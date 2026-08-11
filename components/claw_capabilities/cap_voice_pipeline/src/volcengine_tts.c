/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "voice_pipeline_private.h"

static const char *TAG = "volcengine_tts";

#define TTS_DEFAULT_URL        "https://openspeech.bytedance.com/api/v1/tts"
#define TTS_DEFAULT_VOICE_TYPE "BV001_streaming"
#define TTS_DEFAULT_CLUSTER    "volcano_tts"
#define TTS_SAMPLE_RATE        24000
/** Volcengine returns code 3000 on successful synthesis. */
#define TTS_SUCCESS_CODE       3000
#define TTS_RESP_INITIAL_CAP   (16 * 1024)
/** Guard against a runaway response; 512 KB of base64 is ~6 s of 24 kHz PCM. */
#define TTS_RESP_MAX_BYTES     (512 * 1024)
#define TTS_PLAYBACK_CHUNK     4096
#define TTS_HTTP_TIMEOUT_MS    15000

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
    bool overflow;
} tts_response_buffer_t;

/** Allocate from PSRAM when available, falling back to internal RAM. */
static void *tts_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return ptr ? ptr : malloc(size);
}

static esp_err_t tts_response_append(tts_response_buffer_t *buf, const char *data, size_t len)
{
    if (buf->len + len + 1 > TTS_RESP_MAX_BYTES) {
        if (!buf->overflow) {
            ESP_LOGE(TAG, "TTS response exceeds %d bytes, aborting", TTS_RESP_MAX_BYTES);
            buf->overflow = true;
        }
        return ESP_ERR_NO_MEM;
    }

    if (buf->len + len + 1 > buf->capacity) {
        size_t new_cap = buf->capacity ? buf->capacity * 2 : TTS_RESP_INITIAL_CAP;

        while (new_cap < buf->len + len + 1) {
            new_cap *= 2;
        }
        if (new_cap > TTS_RESP_MAX_BYTES) {
            new_cap = TTS_RESP_MAX_BYTES;
        }

        char *new_ptr = tts_alloc(new_cap);
        if (!new_ptr) {
            ESP_LOGE(TAG, "Failed to grow TTS response buffer to %u bytes", (unsigned)new_cap);
            return ESP_ERR_NO_MEM;
        }
        if (buf->len > 0) {
            memcpy(new_ptr, buf->data, buf->len);
        }
        free(buf->data);
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    tts_response_buffer_t *buf = (tts_response_buffer_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data_len > 0) {
        return tts_response_append(buf, (const char *)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

/** Stream decoded PCM to the speaker in bounded chunks. */
static esp_err_t tts_play_pcm(const uint8_t *pcm, size_t pcm_len)
{
    size_t offset = 0;

    while (offset < pcm_len) {
        size_t chunk = pcm_len - offset;
        size_t written = 0;
        esp_err_t err;

        if (chunk > TTS_PLAYBACK_CHUNK) {
            chunk = TTS_PLAYBACK_CHUNK;
        }

        err = i2s_audio_driver_write(pcm + offset, chunk, &written, 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed after %u bytes: %s", (unsigned)offset,
                     esp_err_to_name(err));
            return err;
        }
        if (written == 0) {
            ESP_LOGE(TAG, "I2S write stalled after %u bytes", (unsigned)offset);
            return ESP_ERR_TIMEOUT;
        }
        offset += written;
    }

    return ESP_OK;
}

/**
 * Decode the base64 `data` field of a Volcengine TTS response and play it.
 *
 * The non-streaming ("query") endpoint answers with a JSON envelope
 * {"code":3000,"message":"Success","data":"<base64 pcm>"}, so the body must be
 * parsed and decoded before it can be handed to the speaker.
 */
static esp_err_t tts_decode_and_play(const char *body, size_t body_len)
{
    cJSON *root;
    cJSON *data_item;
    cJSON *code_item;
    const char *b64;
    size_t b64_len;
    size_t pcm_capacity;
    size_t pcm_len = 0;
    uint8_t *pcm;
    int mbed_ret;
    esp_err_t err;

    if (!body || body_len == 0) {
        ESP_LOGE(TAG, "Empty TTS response body");
        return ESP_ERR_INVALID_RESPONSE;
    }

    root = cJSON_ParseWithLength(body, body_len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse TTS response JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    code_item = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code_item) && code_item->valueint != TTS_SUCCESS_CODE) {
        cJSON *message_item = cJSON_GetObjectItem(root, "message");

        ESP_LOGE(TAG, "TTS service returned code=%d message=%s", code_item->valueint,
                 (cJSON_IsString(message_item) && message_item->valuestring)
                     ? message_item->valuestring
                     : "(none)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    data_item = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data_item) || !data_item->valuestring || !data_item->valuestring[0]) {
        ESP_LOGE(TAG, "TTS response has no base64 'data' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    b64 = data_item->valuestring;
    b64_len = strlen(b64);

    /* base64 expands 3 bytes into 4, so the decoded payload is at most 3/4 of it. */
    pcm_capacity = (b64_len / 4 + 1) * 3;
    pcm = tts_alloc(pcm_capacity);
    if (!pcm) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for decoded PCM", (unsigned)pcm_capacity);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    mbed_ret = mbedtls_base64_decode(pcm, pcm_capacity, &pcm_len, (const unsigned char *)b64,
                                     b64_len);
    cJSON_Delete(root);

    if (mbed_ret != 0) {
        ESP_LOGE(TAG, "base64 decode of TTS audio failed: -0x%04x", -mbed_ret);
        free(pcm);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Playing %u bytes of decoded PCM (~%u ms @%d Hz)", (unsigned)pcm_len,
             (unsigned)(pcm_len * 1000 / (TTS_SAMPLE_RATE * 2)), TTS_SAMPLE_RATE);

    err = tts_play_pcm(pcm, pcm_len);
    free(pcm);
    return err;
}

static char *tts_build_request_body(const app_config_t *config, const char *text,
                                    const char *voice_type)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *app;
    cJSON *user;
    cJSON *audio;
    cJSON *request;
    char *json_body;

    if (!root) {
        return NULL;
    }

    app = cJSON_AddObjectToObject(root, "app");
    if (config->tts_app_id[0]) {
        cJSON_AddStringToObject(app, "appid", config->tts_app_id);
    }
    cJSON_AddStringToObject(app, "token", config->tts_api_key[0] ? config->tts_api_key : "default");
    cJSON_AddStringToObject(app, "cluster", TTS_DEFAULT_CLUSTER);

    user = cJSON_AddObjectToObject(root, "user");
    cJSON_AddStringToObject(user, "uid", "esp_claw_device");

    audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "voice_type", voice_type);
    cJSON_AddStringToObject(audio, "encoding", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", TTS_SAMPLE_RATE);

    request = cJSON_AddObjectToObject(root, "request");
    cJSON_AddStringToObject(request, "reqid", "esp-claw-tts");
    cJSON_AddStringToObject(request, "text", text);
    cJSON_AddStringToObject(request, "operation", "query");

    json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_body;
}

esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text)
{
    const char *url;
    const char *voice_type;
    char *json_body;
    char *auth_hdr = NULL;
    esp_http_client_handle_t client;
    tts_response_buffer_t resp_buf = {0};
    esp_err_t err;

    if (!config || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    url = config->tts_base_url[0] ? config->tts_base_url : TTS_DEFAULT_URL;
    voice_type = config->tts_voice_type[0] ? config->tts_voice_type : TTS_DEFAULT_VOICE_TYPE;

    ESP_LOGI(TAG, "Requesting Volcengine TTS: voice='%s' url='%s' text='%.60s%s'", voice_type, url,
             text, strlen(text) > 60 ? "..." : "");

    json_body = tts_build_request_body(config, text, voice_type);
    if (!json_body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = tts_http_event_handler,
        .user_data = &resp_buf,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
    };

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(json_body);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (config->tts_api_key[0]) {
        /* Kept off the task stack: the token alone can be APP_CONFIG_STR_LEN bytes. */
        size_t auth_len = strlen(config->tts_api_key) + sizeof("Bearer;");

        auth_hdr = malloc(auth_len);
        if (!auth_hdr) {
            esp_http_client_cleanup(client);
            free(json_body);
            return ESP_ERR_NO_MEM;
        }
        /* Volcengine expects the "Bearer;" (semicolon) prefix, not "Bearer ". */
        snprintf(auth_hdr, auth_len, "Bearer;%s", config->tts_api_key);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }

    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "TTS HTTP status=%d, response len=%u", status, (unsigned)resp_buf.len);
        if (resp_buf.overflow) {
            err = ESP_ERR_NO_MEM;
        } else if (status != 200) {
            ESP_LOGE(TAG, "TTS HTTP request rejected with status %d: %.200s", status,
                     resp_buf.data ? resp_buf.data : "(empty)");
            err = ESP_FAIL;
        } else {
            err = tts_decode_and_play(resp_buf.data, resp_buf.len);
        }
    } else {
        ESP_LOGE(TAG, "TTS HTTP perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(resp_buf.data);
    free(auth_hdr);
    free(json_body);
    return err;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
