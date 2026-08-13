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

/* Playback attenuation as a Q15 multiplier. See voice_audio_io_set_volume. */
static int s_volume_percent = VOICE_VOLUME_DEFAULT;
static int32_t s_volume_q15 = 1 << 15;

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

/**
 * Put a channel someone else shut down back the way the board configured it.
 *
 * The board manager hands out one i2s_chan_handle_t per peripheral, so every
 * other user of that peripheral operates on the very handle held here. The
 * Lua audio module is the one that bites: audio.new_output{} opens the codec
 * device, and closing it (explicitly or from __gc) reaches
 * esp_codec_dev_close -> data_if->enable(false) -> i2s_channel_disable() on
 * this channel. Nothing ever re-enables it, so every later read or write
 * fails with ESP_ERR_INVALID_STATE until the board reboots.
 *
 * Opening the codec also reprograms the clock and slots to whatever format
 * the other user asked for, which is why this reapplies the board's own
 * config rather than just calling i2s_channel_enable().
 */
static esp_err_t voice_audio_io_rearm(const char *periph_name, i2s_chan_handle_t chan)
{
    periph_i2s_config_t *cfg = NULL;
    esp_err_t err = esp_board_manager_get_periph_config(periph_name, (void **)&cfg);

    if (err != ESP_OK || !cfg || cfg->mode != I2S_COMM_MODE_STD) {
        ESP_LOGE(TAG, "Cannot re-arm '%s': board config unavailable", periph_name);
        return ESP_ERR_INVALID_STATE;
    }

    /* Reconfiguring is rejected while the channel is running, and the channel
     * may be enabled here if the format alone drifted, so always disable. */
    i2s_channel_disable(chan);

    err = i2s_channel_reconfig_std_clock(chan, &cfg->i2s_cfg.std.clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_slot(chan, &cfg->i2s_cfg.std.slot_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot restore '%s' format: %s", periph_name, esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot re-enable '%s': %s", periph_name, esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Re-armed '%s': another component had disabled the channel", periph_name);
    return ESP_OK;
}

/**
 * @brief True when @p chan is not in the running state.
 *
 * i2s_channel_read/write report a disabled channel and a concurrent transfer
 * on the same channel with the same error, so the distinction has to come
 * from the driver's own view. Only the first case is ours to repair.
 */
static bool voice_audio_io_channel_down(i2s_chan_handle_t chan)
{
    i2s_chan_info_t info = {0};

    if (i2s_channel_get_info(chan, &info) != ESP_OK) {
        return false;
    }
    return !info.is_enabled;
}

esp_err_t voice_audio_io_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    esp_err_t err;

    if (!s_rx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    err = i2s_channel_read(s_rx_chan, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_ERR_INVALID_STATE && voice_audio_io_channel_down(s_rx_chan) &&
        voice_audio_io_rearm(VOICE_PERIPH_MIC, s_rx_chan) == ESP_OK) {
        err = i2s_channel_read(s_rx_chan, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
    }
    return err;
}

esp_err_t voice_audio_io_write(const void *src, size_t size, size_t *bytes_written,
                               uint32_t timeout_ms)
{
    esp_err_t err;

    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    err = i2s_channel_write(s_tx_chan, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_ERR_INVALID_STATE && voice_audio_io_channel_down(s_tx_chan) &&
        voice_audio_io_rearm(VOICE_PERIPH_SPEAKER, s_tx_chan) == ESP_OK) {
        err = i2s_channel_write(s_tx_chan, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Playback volume                                                     */
/* ------------------------------------------------------------------ */

esp_err_t voice_audio_io_set_volume(int percent)
{
    if (percent < 0 || percent > VOICE_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Squaring the fraction approximates a perceptual taper: half volume
     * lands near -12 dB, which sounds like "half" far better than the -6 dB
     * a linear scale would give. Gain never exceeds 1.0, so scaled samples
     * cannot overflow and no clipping guard is needed. */
    s_volume_percent = percent;
    s_volume_q15 = ((int32_t)percent * percent * (1 << 15)) / (VOICE_VOLUME_MAX * VOICE_VOLUME_MAX);
    return ESP_OK;
}

int voice_audio_io_get_volume(void)
{
    return s_volume_percent;
}

void voice_audio_io_apply_volume(int16_t *samples, size_t sample_count)
{
    int32_t gain = s_volume_q15;

    if (gain >= (1 << 15) || !samples) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
    }
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
