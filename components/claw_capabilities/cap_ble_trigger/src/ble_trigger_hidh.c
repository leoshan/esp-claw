/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED && CONFIG_CLAW_CAP_BLE_TRIGGER_HID_HOST

#include <stdio.h>
#include <string.h>

#include "esp_hidh.h"
/* esp_hidh_dev_open is declared per host stack, not in the common header. */
#include "esp_hidh_nimble.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"

#include "ble_trigger_private.h"

static const char *TAG = "ble_trig_hidh";

/* Assigned number for the HID service. A HOGP peripheral must advertise it,
 * which is what makes a button findable without knowing its name. */
#define BLE_TRIGGER_HID_SVC_UUID16 0x1812

#define BLE_TRIGGER_SCAN_ITVL 0x0060 /* 60 ms, in 0.625 ms units. */
#define BLE_TRIGGER_SCAN_WIN  0x0030 /* 30 ms: 50% duty, leaving radio for Wi-Fi. */
#define BLE_TRIGGER_MAX_SEEN  12

typedef struct {
    ble_addr_t addr;
    char name[32];
    int8_t rssi;
    bool is_hid;
} ble_trigger_seen_t;

static bool s_hidh_inited = false;
static bool s_scanning = false;
static bool s_connected = false;
static bool s_pressed = false;
static SemaphoreHandle_t s_scan_done = NULL;
static ble_trigger_seen_t s_seen[BLE_TRIGGER_MAX_SEEN];
static int s_seen_count = 0;
static ble_addr_t s_target;
static bool s_have_target = false;

/**
 * Decide whether a HID input report means "held down".
 *
 * The report layout differs per device and is only knowable from that
 * device's report map, so this deliberately does not decode keycodes: for
 * every HID input report that matters here -- boot keyboard, consumer
 * control, a vendor one-button remote -- an all-zero payload is the idle
 * state and anything else is a key being held. Key-repeat therefore reads as
 * a continued press, which ble_trigger_report already collapses.
 */
static bool ble_trigger_report_is_press(const uint8_t *data, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        if (data[i] != 0) {
            return true;
        }
    }
    return false;
}

static void ble_trigger_hidh_event(void *handler_args, esp_event_base_t base, int32_t id,
                                   void *event_data)
{
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    (void)handler_args;
    (void)base;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "HID button connected");
        break;

    case ESP_HIDH_INPUT_EVENT: {
        bool pressed = ble_trigger_report_is_press(param->input.data, param->input.length);

        ESP_LOGD(TAG, "HID report id=%u len=%u -> %s", param->input.report_id,
                 param->input.length, pressed ? "press" : "release");
        ble_trigger_report(BLE_TRIGGER_SOURCE_HID, pressed);
        s_pressed = pressed;
        break;
    }

    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGW(TAG, "HID button disconnected (reason %d)", param->close.reason);
        s_connected = false;
        /* Releasing on disconnect keeps a dropped link from holding the
         * microphone open until the hold timeout fires. */
        if (s_pressed) {
            ble_trigger_report(BLE_TRIGGER_SOURCE_HID, false);
            s_pressed = false;
        }
        break;

    default:
        break;
    }
}

esp_err_t ble_trigger_hidh_start(void)
{
    esp_hidh_config_t config = {
        .callback = ble_trigger_hidh_event,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    esp_err_t err;

    if (s_hidh_inited) {
        return ESP_OK;
    }
    if (!s_scan_done) {
        s_scan_done = xSemaphoreCreateBinary();
        if (!s_scan_done) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_hidh_inited = true;
    return ESP_OK;
}

void ble_trigger_hidh_stop(void)
{
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (s_hidh_inited) {
        esp_hidh_deinit();
        s_hidh_inited = false;
    }
    s_connected = false;
}

bool ble_trigger_hidh_is_connected(void)
{
    return s_connected;
}

/** Record one advertisement, deduplicating by address. */
static void ble_trigger_note_seen(const struct ble_gap_disc_desc *desc)
{
    struct ble_hs_adv_fields fields = {0};
    ble_trigger_seen_t *slot = NULL;

    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(&s_seen[i].addr, &desc->addr, sizeof(ble_addr_t)) == 0) {
            slot = &s_seen[i];
            break;
        }
    }
    if (!slot) {
        if (s_seen_count >= BLE_TRIGGER_MAX_SEEN) {
            return;
        }
        slot = &s_seen[s_seen_count++];
        memset(slot, 0, sizeof(*slot));
        slot->addr = desc->addr;
    }

    slot->rssi = desc->rssi;

    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        return;
    }
    if (fields.name && fields.name_len) {
        size_t len = fields.name_len < sizeof(slot->name) - 1 ? fields.name_len
                                                              : sizeof(slot->name) - 1;
        memcpy(slot->name, fields.name, len);
        slot->name[len] = '\0';
    }
    for (int i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == BLE_TRIGGER_HID_SVC_UUID16) {
            slot->is_hid = true;
            if (!s_have_target) {
                s_target = desc->addr;
                s_have_target = true;
            }
        }
    }
}

static int ble_trigger_disc_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        ble_trigger_note_seen(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (s_scan_done) {
            xSemaphoreGive(s_scan_done);
        }
        return 0;

    default:
        return 0;
    }
}

/** Render the scan result, so an unknown remote can be told from silence. */
static void ble_trigger_format_report(char *out, size_t size)
{
    size_t used = 0;
    int written = snprintf(out, size, "seen %d device(s)", s_seen_count);

    if (written < 0) {
        return;
    }
    used = (size_t)written;

    for (int i = 0; i < s_seen_count && used < size; i++) {
        const ble_trigger_seen_t *d = &s_seen[i];

        written = snprintf(out + used, size - used,
                           "\n  %02x:%02x:%02x:%02x:%02x:%02x rssi=%d hid=%s name=%s",
                           d->addr.val[5], d->addr.val[4], d->addr.val[3], d->addr.val[2],
                           d->addr.val[1], d->addr.val[0], d->rssi, d->is_hid ? "yes" : "no",
                           d->name[0] ? d->name : "(none)");
        if (written < 0) {
            return;
        }
        used += (size_t)written;
    }
}

esp_err_t ble_trigger_hidh_scan(int duration_sec, char *out_report, size_t report_size)
{
    struct ble_gap_disc_params params = {0};
    int rc;

    if (!s_hidh_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    s_seen_count = 0;
    s_have_target = false;
    if (s_scan_done) {
        (void)xSemaphoreTake(s_scan_done, 0);
    }

    /* Passive: a scan request would make every nearby peripheral answer, which
     * costs airtime this shares with Wi-Fi. The HID service UUID that matters
     * here is in the primary advertisement, not the scan response. */
    params.passive = 1;
    params.itvl = BLE_TRIGGER_SCAN_ITVL;
    params.window = BLE_TRIGGER_SCAN_WIN;
    /* Deduplicating in the controller needs CONFIG_BT_CTRL_BLE_SCAN_DUPL, which
     * costs a table of RAM and is off in this project. Asking for it anyway
     * makes the controller reject the whole scan with HCI 0x07 "memory capacity
     * exceeded" (NimBLE rc=519). ble_trigger_note_seen already deduplicates by
     * address, so the only cost of leaving this off is repeat callbacks. */
    params.filter_duplicates = 0;

    rc = ble_gap_disc(ble_trigger_stack_own_addr_type(), duration_sec * 1000, &params,
                      ble_trigger_disc_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
        return ESP_FAIL;
    }
    s_scanning = true;

    /* +1 s so the controller's own duration expiry wins the race normally. */
    if (s_scan_done &&
        xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(duration_sec * 1000 + 1000)) != pdTRUE) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }

    if (out_report && report_size) {
        ble_trigger_format_report(out_report, report_size);
    }

    if (!s_have_target) {
        ESP_LOGI(TAG, "No BLE HID peripheral found among %d device(s)", s_seen_count);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Opening HID button %02x:%02x:%02x:%02x:%02x:%02x", s_target.val[5],
             s_target.val[4], s_target.val[3], s_target.val[2], s_target.val[1], s_target.val[0]);

    /* Returns the device immediately; the connection outcome arrives later as
     * OPEN_EVENT, so a non-NULL result only means the attempt started. */
    return esp_hidh_dev_open(s_target.val, ESP_HID_TRANSPORT_BLE, s_target.type) ? ESP_OK
                                                                                : ESP_FAIL;
}

#elif CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED /* HID host compiled out */

#include "ble_trigger_private.h"

esp_err_t ble_trigger_hidh_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ble_trigger_hidh_stop(void) {}

esp_err_t ble_trigger_hidh_scan(int duration_sec, char *out_report, size_t report_size)
{
    (void)duration_sec;
    (void)out_report;
    (void)report_size;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_trigger_hidh_is_connected(void)
{
    return false;
}

#endif /* CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED && CONFIG_CLAW_CAP_BLE_TRIGGER_HID_HOST */
