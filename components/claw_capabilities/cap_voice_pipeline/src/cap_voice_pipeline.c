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
#include "claw_event_router.h"
#include "claw_event.h"
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
            ESP_LOGI(TAG, "Recognized Voice Query: %s. Forwarding to esp-claw Event Router...", text_out);

            // Dispatch recognized voice text into esp-claw Agent Event Router
            claw_event_t evt = {
                .type = CLAW_EVENT_IM_MESSAGE,
                .payload = strdup(text_out)
            };
            claw_event_dispatch(&evt);

            // Optional: Speak acknowledgement
            volcengine_tts_speak(s_config, "正在处理您的指令");
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

    // Initialize I2S Hardware
    esp_err_t ret = i2s_audio_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S audio driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "cap_voice_pipeline initialized successfully (ASR: Volcengine SenseVoice, Hardware: Breadboard I2S)");
    return ESP_OK;
}
