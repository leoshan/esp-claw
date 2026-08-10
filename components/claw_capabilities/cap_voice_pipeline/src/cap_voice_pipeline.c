/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice_pipeline.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "cap_im_local.h"
#include "app_config.h"

static const char *TAG = "cap_voice_pipeline";

extern esp_err_t i2s_audio_driver_init(void);
extern esp_err_t i2s_audio_driver_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);
extern esp_err_t volcengine_sensevoice_asr_recognize(const app_config_t *config, const uint8_t *pcm_data, size_t pcm_len, char *out_text, size_t max_text_len);
extern esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text);

static const app_config_t *s_config = NULL;
static bool s_is_listening = false;
static TaskHandle_t s_record_task_handle = NULL;

#define RECORD_DURATION_SEC 4
#define RECORD_SAMPLE_RATE  16000
#define RECORD_BUFFER_SIZE  (RECORD_SAMPLE_RATE * 2 * RECORD_DURATION_SEC) // 16-bit Mono = 2 bytes per sample

/**
 * Outbound callback registered with cap_im_local.
 * When esp-claw Agent Loop completes reasoning and emits the final reply text,
 * this function is invoked to convert the text response into TTS audio playback.
 */
static esp_err_t on_agent_outbound_message_cb(const cap_im_local_message_t *message, void *user_ctx)
{
    (void)user_ctx;
    if (!message || !message->text || message->text[0] == '\0') {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Agent Outbound Response Received: '%s' -> Synthesizing Volcengine TTS Audio...", message->text);

    if (s_config) {
        esp_err_t ret = volcengine_tts_speak(s_config, message->text);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Volcengine TTS playback completed successfully.");
        } else {
            ESP_LOGE(TAG, "Volcengine TTS playback failed: %s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

static void voice_record_asr_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Voice record ASR task started, recording for %d seconds...", RECORD_DURATION_SEC);

    uint8_t *pcm_buffer = malloc(RECORD_BUFFER_SIZE);
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM recording buffer");
        s_is_listening = false;
        vTaskDelete(NULL);
        return;
    }

    size_t total_read = 0;
    while (s_is_listening && total_read < RECORD_BUFFER_SIZE) {
        size_t bytes_read = 0;
        size_t chunk_to_read = 1024;
        if (total_read + chunk_to_read > RECORD_BUFFER_SIZE) {
            chunk_to_read = RECORD_BUFFER_SIZE - total_read;
        }

        esp_err_t ret = i2s_audio_driver_read(pcm_buffer + total_read, chunk_to_read, &bytes_read, 200);
        if (ret == ESP_OK && bytes_read > 0) {
            total_read += bytes_read;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_is_listening = false;
    ESP_LOGI(TAG, "Finished recording %d bytes of PCM audio. Starting Volcengine SenseVoice ASR...", (int)total_read);

    char text_out[512] = {0};
    if (s_config && total_read > 0) {
        esp_err_t asr_err = volcengine_sensevoice_asr_recognize(s_config, pcm_buffer, total_read, text_out, sizeof(text_out));
        if (asr_err == ESP_OK && text_out[0] != '\0') {
            ESP_LOGI(TAG, "SenseVoice ASR Success! Text: '%s'", text_out);
            ESP_LOGI(TAG, "Publishing recognized ASR text into esp-claw Agent Event Router...");

            // 1. Publish recognized user voice query into esp-claw Agent Event Router
            char msg_id[64];
            snprintf(msg_id, sizeof(msg_id), "voice-%lu", (unsigned long)xTaskGetTickCount());

            esp_err_t pub_err = claw_event_router_publish_message(
                "voice_gateway", // Source Capability
                "local",         // Channel
                "voice_user",    // Chat ID
                text_out,        // Recognized text content
                "voice_user",    // Sender ID
                msg_id           // Message ID
            );

            if (pub_err == ESP_OK) {
                ESP_LOGI(TAG, "Successfully published voice message to esp-claw Agent Loop.");
            } else {
                ESP_LOGE(TAG, "Failed to publish voice message to esp-claw: %s", esp_err_to_name(pub_err));
            }
        } else {
            ESP_LOGW(TAG, "ASR Recognition failed or no speech detected.");
            volcengine_tts_speak(s_config, "未能识别到说话内容");
        }
    }

    free(pcm_buffer);
    s_record_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t cap_voice_pipeline_start_listening(void)
{
    if (s_is_listening) {
        ESP_LOGW(TAG, "Already listening.");
        return ESP_ERR_INVALID_STATE;
    }

    s_is_listening = true;
    BaseType_t ret = xTaskCreate(voice_record_asr_task, "v_rec_asr", 6144, NULL, 5, &s_record_task_handle);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t cap_voice_pipeline_stop_listening(void)
{
    s_is_listening = false;
    return ESP_OK;
}

esp_err_t cap_voice_pipeline_speak(const char *text)
{
    if (!s_config || !text) return ESP_ERR_INVALID_ARG;
    return volcengine_tts_speak(s_config, text);
}

esp_err_t cap_voice_pipeline_init(const app_config_t *config)
{
    s_config = config;
    ESP_LOGI(TAG, "Initializing cap_voice_pipeline Capability...");

    // 1. Initialize I2S Hardware
    esp_err_t ret = i2s_audio_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S audio driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Register Outbound Callback to intercept Agent reply text and convert to Volcengine TTS
    cap_im_local_set_outbound_callback(on_agent_outbound_message_cb, NULL);

    ESP_LOGI(TAG, "cap_voice_pipeline initialized successfully (ASR: Volcengine SenseVoice -> Event Router, Response -> Volcengine TTS)");
    return ESP_OK;
}
