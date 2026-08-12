/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status face on a 0.91" 128x32 SSD1306 OLED hanging off the board I2C bus.
 *
 * The panel carries no font of its own and is only four pixel pages tall, so
 * a 5x7 table is drawn here at 3x scale: one short word fills the screen and
 * stays readable at arm's length. Faces were tried first and read as abstract
 * shapes at this size.
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

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

#define FONT_W       5
#define FONT_H       7
#define LABEL_SCALE  3
#define LABEL_GAP    3
#define LABEL_CHAR_W (FONT_W * LABEL_SCALE)
#define LABEL_ADVANCE (LABEL_CHAR_W + LABEL_GAP)
/* Agent messages are longer than a status word, so they get a smaller cell:
 * 2x gives 10 characters across and two rows. */
#define MESSAGE_SCALE 2

/* Classic 5x7 cell, one byte per column, bit 0 = top row. Only the glyphs the
 * status words need are stored. */
static const uint8_t FONT_AZ[26][FONT_W] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
};

static const uint8_t FONT_09[10][FONT_W] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
};

/* Punctuation an agent-supplied string is likely to contain. */
static const struct {
    char c;
    uint8_t cols[FONT_W];
} FONT_PUNCT[] = {
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {',', {0x00, 0x50, 0x30, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'%', {0x63, 0x13, 0x08, 0x64, 0x63}},
    {'*', {0x14, 0x08, 0x3E, 0x08, 0x14}},
    {'=', {0x14, 0x14, 0x14, 0x14, 0x14}},
    {'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
    {')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
};

static const uint8_t FONT_BLANK[FONT_W] = {0x00, 0x00, 0x00, 0x00, 0x00};

/**
 * Look up a glyph. Lowercase folds to uppercase; anything else, including every
 * multi-byte character, has no glyph in a 5x7 table and renders blank.
 */
static const uint8_t *font_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return FONT_AZ[c - 'A'];
    }
    if (c >= '0' && c <= '9') {
        return FONT_09[c - '0'];
    }
    for (size_t i = 0; i < sizeof(FONT_PUNCT) / sizeof(FONT_PUNCT[0]); i++) {
        if (FONT_PUNCT[i].c == c) {
            return FONT_PUNCT[i].cols;
        }
    }
    return FONT_BLANK;
}

/** Draw one glyph with every pixel expanded into a scale x scale square. */
static void fb_char(int x, int y, char c, int scale)
{
    const uint8_t *glyph = font_glyph(c);

    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (glyph[col] & (1u << row)) {
                fb_rect(x + col * scale, y + row * scale, scale, scale);
            }
        }
    }
}

/** Draw text at @p scale, wrapping onto as many rows as the panel allows. */
static void fb_wrapped(const char *text, int scale)
{
    int char_w = FONT_W * scale;
    int advance = char_w + scale;         /* one scaled pixel of tracking */
    int line_h = FONT_H * scale + scale;
    int per_line = OLED_WIDTH / advance;
    int max_lines = OLED_HEIGHT / line_h;
    int line = 0;
    size_t i = 0;
    size_t len = strlen(text);

    if (per_line < 1 || max_lines < 1) {
        return;
    }

    while (i < len && line < max_lines) {
        int col = 0;

        /* A newline or a full row ends the line; spaces at a break are eaten. */
        while (i < len && col < per_line && text[i] != '\n') {
            fb_char(col * advance, line * line_h, text[i], scale);
            col++;
            i++;
        }
        while (i < len && text[i] == ' ') {
            i++;
        }
        if (i < len && text[i] == '\n') {
            i++;
        }
        line++;
    }
}

/** Centre a short word on the panel. */
static void fb_label(const char *text)
{
    size_t len = strlen(text);
    int width = (int)len * LABEL_ADVANCE - LABEL_GAP;
    int x = (OLED_WIDTH - width) / 2;
    int y = (OLED_HEIGHT - FONT_H * LABEL_SCALE) / 2;

    if (x < 0) {
        x = 0;
    }
    for (size_t i = 0; i < len; i++) {
        fb_char(x + (int)i * LABEL_ADVANCE, y, text[i], LABEL_SCALE);
    }
}

static const char *status_label(voice_status_t status)
{
    switch (status) {
    case VOICE_STATUS_LISTENING:
        return "LISTEN";
    case VOICE_STATUS_THINKING:
        return "THINK";
    case VOICE_STATUS_SPEAKING:
        return "TALK";
    case VOICE_STATUS_RETRY:
        return "RETRY?";
    case VOICE_STATUS_IDLE:
    default:
        return "IDLE";
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

esp_err_t voice_status_display_show_text(const char *text)
{
    esp_err_t err;

    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dev || !s_lock) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(OLED_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    fb_clear();
    fb_wrapped(text, MESSAGE_SCALE);
    err = oled_flush();

    /* The message now owns the panel. Forget the cached status so the next
     * state change always redraws, even if it repeats the previous state. */
    s_status_valid = false;

    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Display update failed: %s", esp_err_to_name(err));
    }
    return err;
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
    fb_label(status_label(status));

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
