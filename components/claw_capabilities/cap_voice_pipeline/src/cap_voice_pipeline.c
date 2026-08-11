/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice_pipeline.h"

#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cap_im_local.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "voice_pipeline_private.h"

static const char *TAG = "cap_voice_pipeline";

#define VOICE_RECORD_DURATION_SEC CONFIG_CLAW_CAP_VOICE_RECORD_DURATION_SEC
#define VOICE_ASR_TEXT_MAX        512
/* The ASR client owns a WebSocket, TLS session and PCM packet buffer. */
#define VOICE_RECORD_TASK_STACK   8192
#define VOICE_RECORD_TASK_PRIO    5
#define VOICE_LOCK_TIMEOUT_MS     1000
/* Long enough to outlast an in-flight utterance rather than talk over it. */
#define VOICE_PLAYBACK_LOCK_TIMEOUT_MS 30000
#define VOICE_TTS_QUEUE_DEPTH     4
#define VOICE_TTS_TASK_STACK      8192
#define VOICE_TTS_TASK_PRIO       5
/* Bound on how long deinit waits for the capture and playback tasks. */
#define VOICE_DEINIT_WAIT_MS      5000
#define VOICE_CAP_GROUP_ID        "cap_voice_pipeline"
#define VOICE_CHANNEL             "local"
#define VOICE_CHAT_ID             "voice_user"
#define VOICE_SENDER_ID           "voice_user"
#define VOICE_SOURCE_CAP          "voice_gateway"

typedef struct {
    SemaphoreHandle_t lock;
    /* Serializes speaker access: agent replies and voice_speak tool calls
     * arrive on different tasks and must not interleave on the I2S channel. */
    SemaphoreHandle_t playback_lock;
    /* Owned copy. main.c frees the app_config_t it passes to init() at the end
     * of app_main, so holding the pointer would be a use-after-free. */
    app_config_t config;
    QueueHandle_t tts_queue;
    TaskHandle_t tts_task;
    bool initialized;
    bool listening;
    TaskHandle_t record_task;
} voice_pipeline_state_t;

/** One queued sentence to speak. */
typedef struct {
    char *text;
} voice_tts_request_t;

static voice_pipeline_state_t s_voice = {0};

static esp_err_t voice_lock(void)
{
    if (!s_voice.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_voice.lock, pdMS_TO_TICKS(VOICE_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void voice_unlock(void)
{
    if (s_voice.lock) {
        xSemaphoreGive(s_voice.lock);
    }
}

/** Snapshot the listening flag without holding the lock across the caller. */
static bool voice_is_listening_locked_read(void)
{
    bool listening;

    if (voice_lock() != ESP_OK) {
        return false;
    }
    listening = s_voice.listening;
    voice_unlock();
    return listening;
}

bool cap_voice_pipeline_is_listening(void)
{
    return voice_is_listening_locked_read();
}

/* ------------------------------------------------------------------ */
/* Outbound agent reply -> TTS                                         */
/* ------------------------------------------------------------------ */

/** Synthesize and play one sentence, serialized against other speakers. */
static esp_err_t voice_speak_locked(const char *text)
{
    esp_err_t err;

    if (!s_voice.playback_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_voice.playback_lock, pdMS_TO_TICKS(VOICE_PLAYBACK_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        ESP_LOGW(TAG, "Speaker busy, dropping utterance");
        return ESP_ERR_TIMEOUT;
    }

    err = volcengine_tts_speak(&s_voice.config, text);
    xSemaphoreGive(s_voice.playback_lock);
    return err;
}

/** Drains queued agent replies so no caller blocks on network + playback. */
static void voice_tts_task(void *pvParameters)
{
    voice_tts_request_t request;

    (void)pvParameters;

    while (xQueueReceive(s_voice.tts_queue, &request, portMAX_DELAY) == pdTRUE) {
        if (!request.text) {
            break; /* Sentinel posted by deinit. */
        }
        ESP_LOGI(TAG, "Speaking agent reply: '%.60s%s'", request.text,
                 strlen(request.text) > 60 ? "..." : "");
        voice_speak_locked(request.text);
        free(request.text);
    }

    s_voice.tts_task = NULL;
    vTaskDelete(NULL);
}

/**
 * Observer registered with cap_im_local. Runs after the primary delivery
 * callback (the web chat page), so agent replies still reach every other
 * channel while also being spoken aloud.
 *
 * Only queues the text: synthesis plus playback takes seconds, and this runs
 * on the task that delivers agent replies to every channel.
 */
static esp_err_t on_agent_outbound_message_cb(const cap_im_local_message_t *message, void *user_ctx)
{
    voice_tts_request_t request;

    (void)user_ctx;
    if (!message || !message->text || message->text[0] == '\0') {
        return ESP_OK;
    }
    if (!s_voice.tts_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    request.text = strdup(message->text);
    if (!request.text) {
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_voice.tts_queue, &request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full, dropping reply");
        free(request.text);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Capture + ASR                                                       */
/* ------------------------------------------------------------------ */

static void voice_publish_recognized_text(const char *text)
{
    char msg_id[64];
    esp_err_t err;

    snprintf(msg_id, sizeof(msg_id), "voice-%lu", (unsigned long)xTaskGetTickCount());

    err = claw_event_router_publish_message(VOICE_SOURCE_CAP, VOICE_CHANNEL, VOICE_CHAT_ID, text,
                                            VOICE_SENDER_ID, msg_id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Published voice message to esp-claw agent loop");
    } else {
        ESP_LOGE(TAG, "Failed to publish voice message: %s", esp_err_to_name(err));
    }
}

static void voice_record_asr_task(void *pvParameters)
{
    char *text_out;

    (void)pvParameters;
    ESP_LOGI(TAG, "Voice capture started (max %d s)", VOICE_RECORD_DURATION_SEC);

    /* Kept off the task stack: 512 bytes far exceeds the 128-byte stack budget. */
    text_out = calloc(1, VOICE_ASR_TEXT_MAX);
    if (!text_out) {
        ESP_LOGE(TAG, "Failed to allocate transcript buffer");
        goto cleanup;
    }

    {
        /* The ASR client streams microphone PCM as it is captured and stops
         * when the listening flag clears, so no full-utterance buffer exists. */
        esp_err_t asr_err = volcengine_asr_stream_recognize(
            &s_voice.config, voice_is_listening_locked_read, text_out, VOICE_ASR_TEXT_MAX);

        if (asr_err == ESP_OK && text_out[0] != '\0') {
            ESP_LOGI(TAG, "ASR result: '%s'", text_out);
            voice_publish_recognized_text(text_out);
        } else {
            ESP_LOGW(TAG, "ASR recognition failed or no speech detected: %s",
                     esp_err_to_name(asr_err));
        }
    }

cleanup:
    free(text_out);

    if (voice_lock() == ESP_OK) {
        s_voice.listening = false;
        s_voice.record_task = NULL;
        voice_unlock();
    }
    vTaskDelete(NULL);
}

esp_err_t cap_voice_pipeline_start_listening(void)
{
    esp_err_t err;
    BaseType_t task_err;

    err = voice_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_voice.initialized) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.listening) {
        voice_unlock();
        ESP_LOGW(TAG, "Already listening");
        return ESP_ERR_INVALID_STATE;
    }

    /* Set before creating the task so its first loop check sees the flag. */
    s_voice.listening = true;
    task_err = xTaskCreate(voice_record_asr_task, "v_rec_asr", VOICE_RECORD_TASK_STACK, NULL,
                           VOICE_RECORD_TASK_PRIO, &s_voice.record_task);
    if (task_err != pdPASS) {
        s_voice.listening = false;
        s_voice.record_task = NULL;
        voice_unlock();
        ESP_LOGE(TAG, "Failed to create voice capture task");
        return ESP_ERR_NO_MEM;
    }

    voice_unlock();
    return ESP_OK;
}

esp_err_t cap_voice_pipeline_stop_listening(void)
{
    esp_err_t err = voice_lock();

    if (err != ESP_OK) {
        return err;
    }
    s_voice.listening = false;
    voice_unlock();
    return ESP_OK;
}

esp_err_t cap_voice_pipeline_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_voice.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return voice_speak_locked(text);
}

/* ------------------------------------------------------------------ */
/* Capability descriptors                                              */
/* ------------------------------------------------------------------ */

static esp_err_t cap_voice_execute_listen(const char *input_json,
                                          const claw_cap_call_context_t *ctx, char *output,
                                          size_t output_size)
{
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = cap_voice_pipeline_start_listening();
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"message\":\"listening started\",\"max_duration_sec\":%d}",
             VOICE_RECORD_DURATION_SEC);
    return ESP_OK;
}

static esp_err_t cap_voice_execute_stop(const char *input_json, const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = cap_voice_pipeline_stop_listening();
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true,\"message\":\"listening stopped\"}");
    return ESP_OK;
}

static esp_err_t cap_voice_execute_speak(const char *input_json, const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    cJSON *root;
    cJSON *text_item;
    esp_err_t err;

    (void)ctx;

    root = input_json ? cJSON_Parse(input_json) : NULL;
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return ESP_ERR_INVALID_ARG;
    }

    text_item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring || !text_item->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"missing 'text'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_voice_pipeline_speak(text_item->valuestring);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true,\"message\":\"speech played\"}");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_voice_descriptors[] = {
    {
        .id = "voice_listen",
        .name = "voice_listen",
        .family = "voice",
        .description = "Start recording from the microphone and transcribe the utterance. "
                       "The recognized text is delivered back as a new user message.",
        .kind = CLAW_CAP_KIND_HYBRID,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_EMITS_EVENTS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_voice_execute_listen,
    },
    {
        .id = "voice_stop_listening",
        .name = "voice_stop_listening",
        .family = "voice",
        .description = "Stop an in-progress recording early and transcribe what was captured.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_voice_execute_stop,
    },
    {
        .id = "voice_speak",
        .name = "voice_speak",
        .family = "voice",
        .description = "Speak the given text aloud through the device speaker using TTS.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\","
                             "\"description\":\"Text to speak aloud.\"}},\"required\":[\"text\"]}",
        .execute = cap_voice_execute_speak,
    },
};

static const claw_cap_group_t s_voice_group = {
    .group_id = VOICE_CAP_GROUP_ID,
    .descriptors = s_voice_descriptors,
    .descriptor_count = sizeof(s_voice_descriptors) / sizeof(s_voice_descriptors[0]),
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t cap_voice_pipeline_init(const app_config_t *config)
{
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_voice.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing cap_voice_pipeline");

    if (!s_voice.lock) {
        s_voice.lock = xSemaphoreCreateMutex();
    }
    if (!s_voice.playback_lock) {
        s_voice.playback_lock = xSemaphoreCreateMutex();
    }
    if (!s_voice.tts_queue) {
        s_voice.tts_queue = xQueueCreate(VOICE_TTS_QUEUE_DEPTH, sizeof(voice_tts_request_t));
    }
    if (!s_voice.lock || !s_voice.playback_lock || !s_voice.tts_queue) {
        ESP_LOGE(TAG, "Failed to create voice pipeline sync primitives");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = voice_audio_io_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to bind board audio peripherals: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Own a copy: main.c frees the app_config_t it hands us at the end of
     * app_main, so every later credential read would hit freed memory. */
    memcpy(&s_voice.config, config, sizeof(s_voice.config));

    if (xTaskCreate(voice_tts_task, "v_tts", VOICE_TTS_TASK_STACK, NULL, VOICE_TTS_TASK_PRIO,
                    &s_voice.tts_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS playback task");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* Expose listen/speak so the router, Lua scripts, console and LLM can all
     * drive the pipeline; without this the ASR path would be unreachable. */
    if (!claw_cap_group_exists(s_voice_group.group_id)) {
        err = claw_cap_register_group(&s_voice_group);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register voice capability group: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    /* Observe (never replace) outbound replies so web chat keeps working. */
    err = cap_im_local_add_outbound_observer(on_agent_outbound_message_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to outbound messages: %s", esp_err_to_name(err));
        claw_cap_unregister_group(s_voice_group.group_id, 0);
        goto fail;
    }

    s_voice.initialized = true;
    ESP_LOGI(TAG, "cap_voice_pipeline initialized (ASR -> event router, agent reply -> TTS)");
    return ESP_OK;

fail:
    if (s_voice.tts_task) {
        voice_tts_request_t stop = {.text = NULL};

        xQueueSend(s_voice.tts_queue, &stop, pdMS_TO_TICKS(100));
        s_voice.tts_task = NULL;
    }
    memset(&s_voice.config, 0, sizeof(s_voice.config));
    voice_audio_io_deinit();
    return err;
}

esp_err_t cap_voice_pipeline_deinit(void)
{
    if (!s_voice.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    cap_im_local_remove_outbound_observer(on_agent_outbound_message_cb);
    claw_cap_unregister_group(s_voice_group.group_id, 0);

    cap_voice_pipeline_stop_listening();
    /* Let the capture task observe the flag and exit before tearing down I2S.
     * Bounded so a wedged capture cannot hang shutdown forever. */
    for (int waited_ms = 0; voice_is_listening_locked_read() && waited_ms < VOICE_DEINIT_WAIT_MS;
         waited_ms += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_voice.tts_task) {
        voice_tts_request_t stop = {.text = NULL};

        xQueueSend(s_voice.tts_queue, &stop, pdMS_TO_TICKS(100));
        for (int waited_ms = 0; s_voice.tts_task && waited_ms < VOICE_DEINIT_WAIT_MS;
             waited_ms += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    voice_audio_io_deinit();

    if (voice_lock() == ESP_OK) {
        memset(&s_voice.config, 0, sizeof(s_voice.config));
        s_voice.initialized = false;
        voice_unlock();
    }

    ESP_LOGI(TAG, "cap_voice_pipeline deinitialized");
    return ESP_OK;
}

#else /* !CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */

/* Capability disabled in Kconfig: keep the API linkable but inert. */

esp_err_t cap_voice_pipeline_init(const app_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cap_voice_pipeline_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cap_voice_pipeline_start_listening(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cap_voice_pipeline_stop_listening(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool cap_voice_pipeline_is_listening(void)
{
    return false;
}

esp_err_t cap_voice_pipeline_speak(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
