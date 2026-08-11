/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "periph_i2s.h"

#include "voice_pipeline_private.h"

static const char *TAG = "voice_audio_io";

/* Borrowed from the board manager under a reference; it owns the channels. */
static i2s_chan_handle_t s_rx_chan = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_mic_referenced = false;
static bool s_speaker_referenced = false;
static int s_mic_sample_rate = 0;
static int s_speaker_sample_rate = 0;

/**
 * Read the sample rate the board actually configured for a peripheral.
 *
 * Trusting a Kconfig value here would silently break any board whose YAML
 * disagrees: the ASR request would declare a rate the microphone does not
 * produce (garbage transcript, no error) and TTS audio would play at the
 * wrong speed.
 */
static esp_err_t voice_audio_io_read_sample_rate(const char *periph_name, int *out_rate)
{
    periph_i2s_config_t *cfg = NULL;
    esp_err_t err = esp_board_manager_get_periph_config(periph_name, (void **)&cfg);

    if (err != ESP_OK || !cfg) {
        ESP_LOGE(TAG, "Cannot read config of '%s': %s", periph_name, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    if (cfg->mode != I2S_COMM_MODE_STD) {
        ESP_LOGE(TAG, "Peripheral '%s' is not in I2S standard mode; voice needs std PCM",
                 periph_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_rate = (int)cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
    if (*out_rate <= 0) {
        ESP_LOGE(TAG, "Peripheral '%s' reports an invalid sample rate", periph_name);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t voice_audio_io_init(void)
{
    void *rx = NULL;
    void *tx = NULL;
    esp_err_t err;

    if (s_rx_chan || s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ref_handle (not get_handle) so the peripheral is initialized on demand
     * and stays alive for as long as we hold the reference. */
    err = esp_board_periph_ref_handle(VOICE_PERIPH_MIC, &rx);
    if (err != ESP_OK || !rx) {
        ESP_LOGE(TAG, "Microphone peripheral '%s' unavailable (%s)", VOICE_PERIPH_MIC,
                 esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    s_mic_referenced = true;

    err = esp_board_periph_ref_handle(VOICE_PERIPH_SPEAKER, &tx);
    if (err != ESP_OK || !tx) {
        ESP_LOGE(TAG, "Speaker peripheral '%s' unavailable (%s)", VOICE_PERIPH_SPEAKER,
                 esp_err_to_name(err));
        goto fail;
    }
    s_speaker_referenced = true;

    err = voice_audio_io_read_sample_rate(VOICE_PERIPH_MIC, &s_mic_sample_rate);
    if (err != ESP_OK) {
        goto fail;
    }
    err = voice_audio_io_read_sample_rate(VOICE_PERIPH_SPEAKER, &s_speaker_sample_rate);
    if (err != ESP_OK) {
        goto fail;
    }

    /* The streaming ASR service only accepts 16 kHz, and resampling on-device
     * is not worth the CPU here, so require the board to capture at that rate. */
    if (s_mic_sample_rate != VOICE_ASR_SAMPLE_RATE) {
        ESP_LOGE(TAG,
                 "Microphone '%s' runs at %d Hz but the ASR service requires %d Hz. "
                 "Set sample_rate_hz: %d on that peripheral in board_peripherals.yaml.",
                 VOICE_PERIPH_MIC, s_mic_sample_rate, VOICE_ASR_SAMPLE_RATE, VOICE_ASR_SAMPLE_RATE);
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    s_rx_chan = (i2s_chan_handle_t)rx;
    s_tx_chan = (i2s_chan_handle_t)tx;

    ESP_LOGI(TAG, "Bound board audio: mic '%s' @%d Hz, speaker '%s' @%d Hz", VOICE_PERIPH_MIC,
             s_mic_sample_rate, VOICE_PERIPH_SPEAKER, s_speaker_sample_rate);
    return ESP_OK;

fail:
    voice_audio_io_deinit();
    return err;
}

void voice_audio_io_deinit(void)
{
    s_rx_chan = NULL;
    s_tx_chan = NULL;
    s_mic_sample_rate = 0;
    s_speaker_sample_rate = 0;

    if (s_speaker_referenced) {
        esp_board_periph_unref_handle(VOICE_PERIPH_SPEAKER);
        s_speaker_referenced = false;
    }
    if (s_mic_referenced) {
        esp_board_periph_unref_handle(VOICE_PERIPH_MIC);
        s_mic_referenced = false;
    }
}

int voice_audio_io_get_speaker_sample_rate(void)
{
    return s_speaker_sample_rate;
}

int voice_audio_io_get_mic_sample_rate(void)
{
    return s_mic_sample_rate;
}

esp_err_t voice_audio_io_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_rx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_read(s_rx_chan, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t voice_audio_io_write(const void *src, size_t size, size_t *bytes_written,
                               uint32_t timeout_ms)
{
    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(s_tx_chan, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
