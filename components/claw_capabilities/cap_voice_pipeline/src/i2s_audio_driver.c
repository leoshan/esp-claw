/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"

#include "voice_pipeline_private.h"

static const char *TAG = "i2s_audio_driver";

#define MIC_I2S_PORT I2S_NUM_0
#define SPK_I2S_PORT I2S_NUM_1

static i2s_chan_handle_t rx_chan = NULL;
static i2s_chan_handle_t tx_chan = NULL;

esp_err_t i2s_audio_driver_deinit(void)
{
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    return ESP_OK;
}

/**
 * Refuse to start when any configured pin is already reserved by another
 * driver.
 *
 * The I2S driver itself only logs "GPIO %d is not usable, maybe conflict with
 * others" and then takes the pin anyway (esp_driver_i2s/i2s_common.c), so
 * without this check a mis-wired board silently loses its display, backlight
 * or codec to the voice pipeline while init still reports success.
 *
 * Note this only catches peripherals that reserve their pins. Pins driven as
 * plain GPIO (an LCD reset or D/C line, for example) are not reserved and
 * cannot be detected here, so the board's own pin map remains the source of
 * truth.
 */
static esp_err_t i2s_audio_check_pins_available(const cap_voice_hardware_config_t *hw)
{
    const struct {
        const char *role;
        int pin;
    } pins[] = {
        { "mic WS", hw->mic_ws_pin },     { "mic SCK", hw->mic_sck_pin },
        { "mic DIN", hw->mic_din_pin },   { "speaker DOUT", hw->spk_dout_pin },
        { "speaker BCLK", hw->spk_bclk_pin }, { "speaker LRCK", hw->spk_lrck_pin },
    };
    bool conflict = false;

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i].pin < 0) {
            continue;
        }
        if (esp_gpio_is_reserved(BIT64(pins[i].pin))) {
            ESP_LOGE(TAG, "GPIO %d (%s) is already reserved by another driver", pins[i].pin,
                     pins[i].role);
            conflict = true;
        }
    }

    if (conflict) {
        ESP_LOGE(TAG,
                 "Refusing to initialize: remap the CLAW_CAP_VOICE_* GPIOs in menuconfig to pins "
                 "this board leaves free, or disable CLAW_CAP_VOICE_PIPELINE_ENABLED");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t i2s_audio_driver_init(const cap_voice_hardware_config_t *hw)
{
    esp_err_t ret;

    if (!hw) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rx_chan || tx_chan) {
        ESP_LOGW(TAG, "I2S audio driver already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_audio_check_pins_available(hw);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "Initializing I2S audio driver: mic(ws=%d sck=%d din=%d @%dHz) spk(bclk=%d lrck=%d dout=%d @%dHz)",
             hw->mic_ws_pin, hw->mic_sck_pin, hw->mic_din_pin, hw->sample_rate_input,
             hw->spk_bclk_pin, hw->spk_lrck_pin, hw->spk_dout_pin, hw->sample_rate_output);

    /* 1. Microphone RX channel */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate RX channel: %s", esp_err_to_name(ret));
        goto fail;
    }

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hw->sample_rate_input),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)hw->mic_sck_pin,
            .ws = (gpio_num_t)hw->mic_ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)hw->mic_din_pin,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(rx_chan, &rx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX std mode: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* 2. Speaker TX channel */
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate TX channel: %s", esp_err_to_name(ret));
        goto fail;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hw->sample_rate_output),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)hw->spk_bclk_pin,
            .ws = (gpio_num_t)hw->spk_lrck_pin,
            .dout = (gpio_num_t)hw->spk_dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(tx_chan, &tx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX std mode: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG, "I2S audio driver initialized");
    return ESP_OK;

fail:
    i2s_audio_driver_deinit();
    return ret;
}

esp_err_t i2s_audio_driver_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!rx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_read(rx_chan, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t i2s_audio_driver_write(const void *src, size_t size, size_t *bytes_written,
                                 uint32_t timeout_ms)
{
    if (!tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(tx_chan, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
