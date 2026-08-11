/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "cap_voice_pipeline.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the I2S RX (microphone) and TX (speaker) channels.
 *
 * @param hw Pin and sample-rate configuration; must stay valid only for the
 *           duration of the call.
 */
esp_err_t i2s_audio_driver_init(const cap_voice_hardware_config_t *hw);

/**
 * @brief Release both I2S channels allocated by i2s_audio_driver_init().
 */
esp_err_t i2s_audio_driver_deinit(void);

esp_err_t i2s_audio_driver_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);

esp_err_t i2s_audio_driver_write(const void *src, size_t size, size_t *bytes_written,
                                 uint32_t timeout_ms);

/**
 * @brief Send PCM audio to the Volcengine SenseVoice ASR endpoint.
 *
 * @param out_text     Caller-owned buffer receiving the recognized text.
 * @param max_text_len Size of @p out_text in bytes.
 */
esp_err_t volcengine_sensevoice_asr_recognize(const app_config_t *config,
                                              const uint8_t *pcm_data,
                                              size_t pcm_len,
                                              char *out_text,
                                              size_t max_text_len);

/**
 * @brief Synthesize @p text through Volcengine TTS and play it on the speaker.
 */
esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text);

#ifdef __cplusplus
}
#endif
