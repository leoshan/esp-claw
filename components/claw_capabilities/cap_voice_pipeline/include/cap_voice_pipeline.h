/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int mic_ws_pin;
    int mic_sck_pin;
    int mic_din_pin;
    int spk_dout_pin;
    int spk_bclk_pin;
    int spk_lrck_pin;
    int sample_rate_input;
    int sample_rate_output;
} cap_voice_hardware_config_t;

/**
 * @brief Initialize voice pipeline capability
 */
esp_err_t cap_voice_pipeline_init(const app_config_t *config);

/**
 * @brief Trigger ASR listening and recognition
 */
esp_err_t cap_voice_pipeline_start_listening(void);

/**
 * @brief Stop listening and process audio buffer for ASR
 */
esp_err_t cap_voice_pipeline_stop_listening(void);

/**
 * @brief Request TTS synthesis and audio playback
 */
esp_err_t cap_voice_pipeline_speak(const char *text);

#ifdef __cplusplus
}
#endif
