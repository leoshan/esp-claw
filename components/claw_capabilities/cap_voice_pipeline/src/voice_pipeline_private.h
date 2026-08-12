/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "cap_voice_pipeline.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board-manager peripheral names declared in board_peripherals.yaml. */
#define VOICE_PERIPH_MIC     "i2s_audio_in"
#define VOICE_PERIPH_SPEAKER "i2s_audio_out"
#define VOICE_PERIPH_I2C     "i2c_master"

/** What the status display is showing. */
typedef enum {
    VOICE_STATUS_IDLE = 0,  /**< Waiting for someone to talk. */
    VOICE_STATUS_LISTENING, /**< Microphone open, streaming to the recognizer. */
    VOICE_STATUS_THINKING,  /**< Transcript handed to the agent, reply pending. */
    VOICE_STATUS_SPEAKING,  /**< Playing the reply through the speaker. */
} voice_status_t;

/**
 * @brief Bring up the optional SSD1306 status OLED on the board's I2C bus.
 *
 * The display is a convenience, not a requirement: a board without an
 * `i2c_master` peripheral or without a panel answering at the configured
 * address simply runs without it.
 */
esp_err_t voice_status_display_init(void);

void voice_status_display_deinit(void);

/** @brief Draw the face for @p status. Cheap and safe to call when unchanged. */
void voice_status_display_set(voice_status_t status);

/* The ASR service only accepts 16 kHz 16-bit mono PCM. */
#define VOICE_ASR_SAMPLE_RATE 16000
#define VOICE_ASR_BITS        16
#define VOICE_ASR_CHANNELS    1

/**
 * @brief Bind to the board-managed I2S microphone and speaker channels.
 *
 * The channels are created and enabled by the board manager from
 * board_peripherals.yaml, so this only looks up the handles. Nothing here
 * touches GPIOs, which is what makes pin conflicts structurally impossible.
 */
esp_err_t voice_audio_io_init(void);

/** @brief Drop the borrowed channel handles. The board manager owns them. */
void voice_audio_io_deinit(void);

esp_err_t voice_audio_io_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);

esp_err_t voice_audio_io_write(const void *src, size_t size, size_t *bytes_written,
                               uint32_t timeout_ms);

/** @brief Sample rate the board configured the speaker channel with, in Hz. */
int voice_audio_io_get_speaker_sample_rate(void);

/** @brief Sample rate the board configured the microphone channel with, in Hz. */
int voice_audio_io_get_mic_sample_rate(void);

/**
 * @brief Pull one utterance through the streaming ASR service.
 *
 * Opens a WebSocket to the Volcengine streaming ASR endpoint, streams PCM read
 * from the microphone until @p should_continue returns false or the recording
 * cap is reached, then returns the final transcript.
 *
 * @param config          Application config supplying the API key and endpoint.
 * @param should_continue Polled between packets; return false to stop early.
 * @param out_text        Caller-owned buffer receiving the recognized text.
 * @param max_text_len    Size of @p out_text in bytes.
 */
esp_err_t volcengine_asr_stream_recognize(const app_config_t *config,
                                          bool (*should_continue)(void),
                                          char *out_text,
                                          size_t max_text_len);

/**
 * @brief Synthesize @p text and stream it straight to the speaker.
 */
esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text);

#ifdef __cplusplus
}
#endif
