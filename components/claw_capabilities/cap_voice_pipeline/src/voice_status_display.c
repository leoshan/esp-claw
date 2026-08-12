/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status face on a 0.91" 128x32 SSD1306 OLED hanging off the board I2C bus.
 *
 * The panel has no font beyond what we draw ourselves and is only four pixel
 * pages tall, so states are shown as faces built from rectangles and discs
 * rather than as text: a glance tells you whether the device is idle,
 * listening, thinking or talking.
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "voice_pipeline_private.h"

static const char *TAG = "voice_status_display";

/* 0.91" modules ship strapped to either address; probe for the one present. */
#define OLED_ADDR_PRIMARY   0x3C
#define OLED_ADDR_SECONDARY 0x3D
#define OLED_PROBE_TIMEOUT_MS 50
#define OLED_CLOCK_HZ    400000
#define OLED_WIDTH       128
#define OLED_HEIGHT      32
#define OLED_PAGES       (OLED_HEIGHT / 8)
#define OLED_TIMEOUT_MS  100
#define OLED_LOCK_TIMEOUT_MS 500
/* SSD1306 co-processor prefixes: 0x00 introduces commands, 0x40 pixel data. */
#define OLED_CTRL_CMD    0x00
#define OLED_CTRL_DATA   0x40

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_bus_referenced = false;
/* The capture task and the playback task both report status, and they overlap
 * when the button is pressed while a reply is still being spoken. */
static SemaphoreHandle_t s_lock = NULL;
/* One bit per pixel, 8 vertically-adjacent pixels per byte (SSD1306 pages). */
static uint8_t s_fb[OLED_WIDTH * OLED_PAGES];
static voice_status_t s_status = VOICE_STATUS_IDLE;
static bool s_status_valid = false;

/* ------------------------------------------------------------------ */
/* Panel transport                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t oled_send(uint8_t control, const uint8_t *payload, size_t len)
{
    uint8_t buf[1 + OLED_WIDTH];

    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = control;
    memcpy(buf + 1, payload, len);
    return i2c_master_transmit(s_dev, buf, len + 1, OLED_TIMEOUT_MS);
}

static esp_err_t oled_command(uint8_t cmd)
{
    return oled_send(OLED_CTRL_CMD, &cmd, 1);
}

static esp_err_t oled_init_panel(void)
{
    /* Standard 128x32 bring-up. The two values that differ from the 128x64
     * default are the multiplex ratio (0x1F) and the COM pin layout (0x02). */
    static const uint8_t init_seq[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide / oscillator */
        0xA8, 0x1F,       /* multiplex ratio = 32 - 1 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1,             /* segment remap, so column 0 is on the left */
        0xC8,             /* reversed COM scan, so row 0 is on top */
        0xDA, 0x02,       /* COM pin layout for a 32-row panel */
        0x81, 0x8F,       /* contrast */
        0xD9, 0xF1,       /* pre-charge */
        0xDB, 0x40,       /* VCOMH deselect */
        0xA4,             /* follow RAM, not all-on */
        0xA6,             /* non-inverted */
        0xAF,             /* display on */
    };

    for (size_t i = 0; i < sizeof(init_seq); i++) {
        esp_err_t err = oled_command(init_seq[i]);

        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t oled_flush(void)
{
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        esp_err_t err;

        if ((err = oled_command((uint8_t)(0xB0 + page))) != ESP_OK || /* page address */
            (err = oled_command(0x00)) != ESP_OK ||                   /* column low nibble */
            (err = oled_command(0x10)) != ESP_OK) {                   /* column high nibble */
            return err;
        }
        err = oled_send(OLED_CTRL_DATA, s_fb + page * OLED_WIDTH, OLED_WIDTH);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_pixel(int x, int y)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    s_fb[(y / 8) * OLED_WIDTH + x] |= (uint8_t)(1u << (y % 8));
}

static void fb_rect(int x, int y, int w, int h)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            fb_pixel(x + dx, y + dy);
        }
    }
}

static void fb_disc(int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                fb_pixel(cx + dx, cy + dy);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Faces                                                               */
/* ------------------------------------------------------------------ */

#define EYE_LEFT_X   46
#define EYE_RIGHT_X  82
#define EYE_Y        11
#define MOUTH_CX     64
#define MOUTH_Y      23

/** Sleeping: both eyes closed, with rising "z" strokes beside the head. */
static void draw_idle(void)
{
    fb_rect(EYE_LEFT_X - 6, EYE_Y, 13, 2);
    fb_rect(EYE_RIGHT_X - 6, EYE_Y, 13, 2);
    fb_rect(MOUTH_CX - 5, MOUTH_Y, 11, 2);

    /* Two "z" glyphs of different sizes, drawn as three strokes each. */
    fb_rect(104, 8, 9, 2);
    fb_rect(104, 14, 9, 2);
    for (int i = 0; i < 6; i++) {
        fb_pixel(111 - i, 9 + i);
    }
    fb_rect(116, 2, 6, 2);
    fb_rect(116, 6, 6, 2);
    for (int i = 0; i < 4; i++) {
        fb_pixel(121 - i, 3 + i);
    }
}

/** Listening: wide eyes and a level meter suggesting incoming sound. */
static void draw_listening(void)
{
    fb_disc(EYE_LEFT_X, EYE_Y + 1, 4);
    fb_disc(EYE_RIGHT_X, EYE_Y + 1, 4);
    fb_rect(MOUTH_CX - 4, MOUTH_Y, 9, 3);

    static const int bar_heights[] = {6, 14, 22, 14, 6};
    for (int i = 0; i < 5; i++) {
        int h = bar_heights[i];

        fb_rect(6 + i * 4, (OLED_HEIGHT - h) / 2, 2, h);
        fb_rect(110 + i * 3, (OLED_HEIGHT - h) / 2, 2, h);
    }
}

/** Thinking: eyes glancing up, with trailing dots. */
static void draw_thinking(void)
{
    fb_rect(EYE_LEFT_X - 5, EYE_Y + 4, 11, 2);
    fb_rect(EYE_RIGHT_X - 5, EYE_Y + 4, 11, 2);
    fb_disc(EYE_LEFT_X, EYE_Y - 1, 2);
    fb_disc(EYE_RIGHT_X, EYE_Y - 1, 2);
    fb_rect(MOUTH_CX - 3, MOUTH_Y, 7, 2);

    for (int i = 0; i < 3; i++) {
        fb_disc(102 + i * 9, 16, 2);
    }
}

/** Speaking: open eyes and an open mouth. */
static void draw_speaking(void)
{
    fb_disc(EYE_LEFT_X, EYE_Y, 4);
    fb_disc(EYE_RIGHT_X, EYE_Y, 4);
    fb_disc(MOUTH_CX, MOUTH_Y + 1, 6);

    /* Sound radiating from the right side. */
    for (int i = 0; i < 3; i++) {
        fb_rect(104 + i * 7, 14 - i * 4, 3, 5 + i * 8);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t voice_status_display_init(void)
{
    void *bus = NULL;
    uint16_t addr;
    esp_err_t err;

    if (s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_board_periph_ref_handle(VOICE_PERIPH_I2C, &bus);
    if (err != ESP_OK || !bus) {
        ESP_LOGI(TAG, "No '%s' peripheral; running without a status display",
                 VOICE_PERIPH_I2C);
        return ESP_ERR_NOT_FOUND;
    }
    s_bus_referenced = true;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    /* Ask the bus which address is populated instead of assuming one. */
    addr = OLED_ADDR_PRIMARY;
    err = i2c_master_probe((i2c_master_bus_handle_t)bus, addr, OLED_PROBE_TIMEOUT_MS);
    if (err != ESP_OK) {
        addr = OLED_ADDR_SECONDARY;
        err = i2c_master_probe((i2c_master_bus_handle_t)bus, addr, OLED_PROBE_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No SSD1306 at 0x%02X or 0x%02X; running without a status display",
                 OLED_ADDR_PRIMARY, OLED_ADDR_SECONDARY);
        goto fail;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = OLED_CLOCK_HZ,
    };

    err = i2c_master_bus_add_device((i2c_master_bus_handle_t)bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot add OLED at 0x%02X: %s", addr, esp_err_to_name(err));
        s_dev = NULL;
        goto fail;
    }

    err = oled_init_panel();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 at 0x%02X did not accept init: %s", addr, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        goto fail;
    }

    s_status_valid = false;
    voice_status_display_set(VOICE_STATUS_IDLE);
    ESP_LOGI(TAG, "Status display ready on %s at 0x%02X", VOICE_PERIPH_I2C, addr);
    return ESP_OK;

fail:
    if (s_bus_referenced) {
        esp_board_periph_unref_handle(VOICE_PERIPH_I2C);
        s_bus_referenced = false;
    }
    return err;
}

void voice_status_display_deinit(void)
{
    if (s_dev) {
        fb_clear();
        oled_flush();
        oled_command(0xAE); /* display off */
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus_referenced) {
        esp_board_periph_unref_handle(VOICE_PERIPH_I2C);
        s_bus_referenced = false;
    }
    s_status_valid = false;
}

void voice_status_display_set(voice_status_t status)
{
    esp_err_t err;

    if (!s_dev || !s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(OLED_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    if (s_status_valid && s_status == status) {
        xSemaphoreGive(s_lock);
        return;
    }

    fb_clear();
    switch (status) {
    case VOICE_STATUS_LISTENING:
        draw_listening();
        break;
    case VOICE_STATUS_THINKING:
        draw_thinking();
        break;
    case VOICE_STATUS_SPEAKING:
        draw_speaking();
        break;
    case VOICE_STATUS_IDLE:
    default:
        draw_idle();
        break;
    }

    err = oled_flush();
    if (err == ESP_OK) {
        s_status = status;
        s_status_valid = true;
    } else {
        ESP_LOGW(TAG, "Display update failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
