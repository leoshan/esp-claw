/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "app_config.h"

static const char *TAG = "volcengine_tts";

extern esp_err_t i2s_audio_driver_write(const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data_len > 0) {
            size_t bytes_written = 0;
            i2s_audio_driver_write(evt->data, evt->data_len, &bytes_written, 1000);
        }
    }
    return ESP_OK;
}

esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text)
{
    if (!config || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *url = config->tts_base_url[0] ? config->tts_base_url : "https://openspeech.bytedance.com/api/v1/tts";
    const char *voice_type = config->tts_voice_type[0] ? config->tts_voice_type : "BV001_streaming";

    ESP_LOGI(TAG, "Requesting Volcengine TTS: text='%s', voice='%s', url='%s'", text, voice_type, url);

    // Build JSON request body
    cJSON *root = cJSON_CreateObject();
    cJSON *app = cJSON_CreateObject();
    if (config->tts_app_id[0]) {
        cJSON_AddStringToObject(app, "appid", config->tts_app_id);
    }
    cJSON_AddStringToObject(app, "token", config->tts_api_key[0] ? config->tts_api_key : "default");
    cJSON_AddStringToObject(app, "cluster", "volcano_tts");
    cJSON_AddItemToObject(root, "app", app);

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "uid", "esp_claw_device");
    cJSON_AddItemToObject(root, "user", user);

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "voice_type", voice_type);
    cJSON_AddStringToObject(audio, "encoding", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", 24000);
    cJSON_AddItemToObject(root, "audio", audio);

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "text", text);
    cJSON_AddStringToObject(request, "operation", "query");
    cJSON_AddItemToObject(root, "request", request);

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = tts_http_event_handler,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        free(json_body);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (config->tts_api_key[0]) {
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", config->tts_api_key);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }

    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS HTTP perform completed with status = %d", status);
    } else {
        ESP_LOGE(TAG, "TTS HTTP perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(json_body);
    return err;
}
