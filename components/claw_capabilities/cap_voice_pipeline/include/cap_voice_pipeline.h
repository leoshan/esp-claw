/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the voice pipeline: bind to the board-managed I2S
 *        microphone and speaker, register the `voice_listen` / `voice_speak`
 *        capabilities, and subscribe to outbound agent replies for TTS.
 *
 * Audio pins come from the board's `i2s_audio_in` / `i2s_audio_out` entries in
 * board_peripherals.yaml, so this capability never touches GPIOs itself.
 *
 * @param config Application config providing ASR/TTS credentials. Must remain
 *               valid for the lifetime of the capability.
 */
esp_err_t cap_voice_pipeline_init(const app_config_t *config);

/**
 * @brief Release the voice pipeline: unsubscribe, stop any recording task and
 *        tear down I2S.
 */
esp_err_t cap_voice_pipeline_deinit(void);

/**
 * @brief Start recording and run ASR on the captured audio.
 *
 * Returns as soon as the capture task is running; the recognized text is
 * published into the event router asynchronously.
 */
esp_err_t cap_voice_pipeline_start_listening(void);

/**
 * @brief Ask the running capture task to stop early and process what it has.
 */
esp_err_t cap_voice_pipeline_stop_listening(void);

/**
 * @brief Report whether a capture/ASR cycle is currently running.
 */
bool cap_voice_pipeline_is_listening(void);

/**
 * @brief Synthesize @p text through TTS and play it on the speaker.
 */
esp_err_t cap_voice_pipeline_speak(const char *text);

#ifdef __cplusplus
}
#endif
