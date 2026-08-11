/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "voice_pipeline_private.h"

static const char *TAG = "volcengine_asr";

#define ASR_DEFAULT_URL         "https://openspeech.bytedance.com/api/v1/asr"
#define ASR_DEFAULT_RESOURCE_ID "volc.sensevoice.v1"
#define ASR_SAMPLE_RATE         16000
#define ASR_WAV_HEADER_SIZE     44

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buffer_t *buf = (response_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf && evt->data_len > 0) {
            if (buf->len + evt->data_len >= buf->capacity) {
                size_t new_cap = (buf->capacity + evt->data_len + 1024) * 2;
                char *new_ptr = realloc(buf->data, new_cap);
                if (!new_ptr) return ESP_ERR_NO_MEM;
                buf->data = new_ptr;
                buf->capacity = new_cap;
            }
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

// Generate standard WAV header for 16-bit MONO PCM at ASR_SAMPLE_RATE
static void generate_wav_header(uint8_t *header, uint32_t pcm_len)
{
    uint32_t total_len = pcm_len + 36;
    uint32_t sample_rate = ASR_SAMPLE_RATE;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &total_len, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1; // PCM
    memcpy(header + 16, &subchunk1_size, 4);
    memcpy(header + 20, &audio_format, 2);
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits_per_sample, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &pcm_len, 4);
}

esp_err_t volcengine_sensevoice_asr_recognize(const app_config_t *config, const uint8_t *pcm_data, size_t pcm_len, char *out_text, size_t max_text_len)
{
    if (!config || !pcm_data || pcm_len == 0 || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *url = config->asr_base_url[0] ? config->asr_base_url : ASR_DEFAULT_URL;
    const char *resource_id = config->asr_resource_id[0] ? config->asr_resource_id : ASR_DEFAULT_RESOURCE_ID;
    char *auth_hdr = NULL;

    out_text[0] = '\0';

    ESP_LOGI(TAG, "Recognizing ASR using Volcengine SenseVoice API: %s (PCM size: %d bytes)", url, (int)pcm_len);

    // Build WAV payload in memory
    size_t wav_len = pcm_len + ASR_WAV_HEADER_SIZE;
    uint8_t *wav_payload = malloc(wav_len);
    if (!wav_payload) {
        ESP_LOGE(TAG, "Failed to allocate WAV buffer");
        return ESP_ERR_NO_MEM;
    }
    generate_wav_header(wav_payload, pcm_len);
    memcpy(wav_payload + ASR_WAV_HEADER_SIZE, pcm_data, pcm_len);

    response_buffer_t resp_buf = {
        .data = malloc(2048),
        .len = 0,
        .capacity = 2048
    };
    if (!resp_buf.data) {
        free(wav_payload);
        return ESP_ERR_NO_MEM;
    }
    resp_buf.data[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp_buf,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(wav_payload);
        free(resp_buf.data);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    if (config->asr_api_key[0]) {
        /* Kept off the task stack: the token alone can be APP_CONFIG_STR_LEN bytes. */
        size_t auth_len = strlen(config->asr_api_key) + sizeof("Bearer;");

        auth_hdr = malloc(auth_len);
        if (!auth_hdr) {
            esp_http_client_cleanup(client);
            free(wav_payload);
            free(resp_buf.data);
            return ESP_ERR_NO_MEM;
        }
        /* Volcengine expects the "Bearer;" (semicolon) prefix, not "Bearer ". */
        snprintf(auth_hdr, auth_len, "Bearer;%s", config->asr_api_key);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
    if (config->asr_app_id[0]) {
        esp_http_client_set_header(client, "X-Api-App-Key", config->asr_app_id);
    }
    esp_http_client_set_header(client, "X-Api-Resource-Id", resource_id);

    esp_http_client_set_post_field(client, (const char *)wav_payload, wav_len);

    esp_err_t err = esp_http_client_perform(client);
    free(wav_payload);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "ASR HTTP Status = %d, response len = %d", status_code, (int)resp_buf.len);

        if (status_code == 200 && resp_buf.len > 0) {
            cJSON *root = cJSON_Parse(resp_buf.data);
            if (root) {
                cJSON *result = cJSON_GetObjectItem(root, "result");
                if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
                    cJSON *first_item = cJSON_GetArrayItem(result, 0);
                    cJSON *text_item = cJSON_GetObjectItem(first_item, "text");
                    if (text_item && text_item->valuestring) {
                        snprintf(out_text, max_text_len, "%s", text_item->valuestring);
                        ESP_LOGI(TAG, "SenseVoice ASR Recognized Text: %s", out_text);
                    }
                } else {
                    cJSON *text_direct = cJSON_GetObjectItem(root, "text");
                    if (text_direct && text_direct->valuestring) {
                        snprintf(out_text, max_text_len, "%s", text_direct->valuestring);
                        ESP_LOGI(TAG, "SenseVoice ASR Text: %s", out_text);
                    }
                }
                cJSON_Delete(root);
            }
        }
    } else {
        ESP_LOGE(TAG, "ASR HTTP Perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(resp_buf.data);
    free(auth_hdr);
    return (out_text[0] != '\0') ? ESP_OK : ESP_FAIL;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
