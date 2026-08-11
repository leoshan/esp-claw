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
 * @brief I2S pin and sample-rate configuration for the voice pipeline.
 *
 * Populated from Kconfig by cap_voice_pipeline_init(); boards that need
 * different wiring override the CLAW_CAP_VOICE_* options in sdkconfig.
 */
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
 * @brief Initialize the voice pipeline: bring up I2S, register the
 *        `voice_listen` / `voice_speak` capabilities, and subscribe to
 *        outbound agent replies for TTS playback.
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
