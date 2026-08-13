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
    VOICE_STATUS_RETRY,     /**< Nothing usable was captured; ask for a repeat. */
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

/** @brief Draw the label for @p status. Cheap and safe to call when unchanged. */
void voice_status_display_set(voice_status_t status);

/**
 * @brief Draw caller-supplied text, wrapped, until the next state change.
 *
 * The 5x7 table holds ASCII only, so any multi-byte character renders blank.
 *
 * @return ESP_ERR_NOT_FOUND when no panel was detected.
 */
esp_err_t voice_status_display_show_text(const char *text);

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

/* Playback volume is applied in software. The reference board drives a
 * MAX98357A, a fixed-gain class-D amplifier with no control interface, so the
 * board manager builds a dummy codec whose set_vol is NULL and installs no
 * software volume handler -- esp_codec_dev_set_out_vol() there returns
 * NOT_SUPPORT and changes nothing. Scaling the PCM before it reaches the DMA
 * is the only volume control this class of board has. */
#define VOICE_VOLUME_MAX     100
#define VOICE_VOLUME_DEFAULT 100

/** @brief Set playback volume, 0 (silent) to VOICE_VOLUME_MAX (unattenuated). */
esp_err_t voice_audio_io_set_volume(int percent);

int voice_audio_io_get_volume(void);

/** @brief Attenuate @p sample_count 16-bit mono samples in place. */
void voice_audio_io_apply_volume(int16_t *samples, size_t sample_count);

/* Four seconds of audio. Sized from what a stalled consumer actually costs:
 * the WebSocket handshake plus the first burst of sends left the microphone
 * unread for over 1.5 s on hardware, and anything not buffered in that window
 * is speech the recognizer never sees. */
#define VOICE_CAPTURE_RING_BYTES (VOICE_ASR_SAMPLE_RATE * 2 * 4)

/**
 * @brief Start draining the microphone into a ring buffer on its own task.
 *
 * Decouples capture from whatever consumes it. The I2S DMA holds about 90 ms,
 * so any consumer that pauses longer than that -- a TLS write, a handshake --
 * loses audio unless something else is reading meanwhile.
 */
esp_err_t voice_capture_start(void);

/** @brief Stop the reader and wait for it to leave the I2S channel. */
void voice_capture_stop(void);

/**
 * @brief Copy up to @p max_bytes of captured audio out of the ring.
 *
 * @param timeout_ms How long to wait for the first bytes; later bytes already
 *                   buffered are taken without waiting.
 * @return Bytes copied, 0 if none arrived within the timeout.
 */
size_t voice_capture_read(void *dest, size_t max_bytes, uint32_t timeout_ms);

/** @brief Discard buffered audio, so a new utterance does not inherit it. */
void voice_capture_flush(void);

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
