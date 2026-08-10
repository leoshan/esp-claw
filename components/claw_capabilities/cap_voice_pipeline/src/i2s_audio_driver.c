/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

static const char *TAG = "i2s_audio_driver";

// Default breadboard pin mappings (matching xiaozhi-esp32 bread-compact-wifi)
#define MIC_I2S_PORT       I2S_NUM_0
#define MIC_GPIO_WS        GPIO_NUM_4
#define MIC_GPIO_SCK       GPIO_NUM_5
#define MIC_GPIO_DIN       GPIO_NUM_6

#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_GPIO_DOUT      GPIO_NUM_7
#define SPK_GPIO_BCLK      GPIO_NUM_15
#define SPK_GPIO_LRCK      GPIO_NUM_16

static i2s_chan_handle_t rx_chan = NULL;
static i2s_chan_handle_t tx_chan = NULL;

esp_err_t i2s_audio_driver_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Initializing I2S audio driver for Breadboard Hardware...");

    // 1. Configure Microphone RX Channel
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate RX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_GPIO_SCK,
            .ws = MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_GPIO_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(rx_chan, &rx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX std mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Configure Speaker TX Channel
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_GPIO_BCLK,
            .ws = SPK_GPIO_LRCK,
            .dout = SPK_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(tx_chan, &tx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX std mode: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_channel_enable(rx_chan);
    i2s_channel_enable(tx_chan);

    ESP_LOGI(TAG, "I2S Mic (WS=%d SCK=%d DIN=%d) and Spk (BCLK=%d LRCK=%d DOUT=%d) initialized",
             MIC_GPIO_WS, MIC_GPIO_SCK, MIC_GPIO_DIN, SPK_GPIO_BCLK, SPK_GPIO_LRCK, SPK_GPIO_DOUT);
    return ESP_OK;
}

esp_err_t i2s_audio_driver_read(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!rx_chan) return ESP_ERR_INVALID_STATE;
    return i2s_channel_read(rx_chan, dest, size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t i2s_audio_driver_write(const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!tx_chan) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(tx_chan, src, size, bytes_written, pdMS_TO_TICKS(timeout_ms));
}
