/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_TRIGGER_DEVICE_NAME    CONFIG_CLAW_CAP_BLE_TRIGGER_DEVICE_NAME
#define BLE_TRIGGER_HOLD_TIMEOUT_MS CONFIG_CLAW_CAP_BLE_TRIGGER_HOLD_TIMEOUT_MS

/* Values a client writes to the trigger characteristic. PRESS/RELEASE mirror
 * a physical button so hold-to-talk works; TAP exists because many BLE remotes
 * (and a person poking a phone app) only ever emit one event per push. */
#define BLE_TRIGGER_CMD_RELEASE 0x00
#define BLE_TRIGGER_CMD_PRESS   0x01
#define BLE_TRIGGER_CMD_TAP     0x02

/* Custom 128-bit UUIDs: this is a private profile, so no assigned number
 * applies. NimBLE's initializer takes the bytes least-significant first,
 * which is the reverse of how the UUID reads in a phone app.
 *
 *   service         7a3f1e00-5c2b-4a9d-8e11-2f6b0c9d4a71
 *   trigger char    7a3f1e01-5c2b-4a9d-8e11-2f6b0c9d4a71  write, notify
 */
#define BLE_TRIGGER_SVC_UUID                                                                     \
    BLE_UUID128_INIT(0x71, 0x4a, 0x9d, 0x0c, 0x6b, 0x2f, 0x11, 0x8e, 0x9d, 0x4a, 0x2b, 0x5c,     \
                     0x00, 0x1e, 0x3f, 0x7a)
#define BLE_TRIGGER_CHR_UUID                                                                     \
    BLE_UUID128_INIT(0x71, 0x4a, 0x9d, 0x0c, 0x6b, 0x2f, 0x11, 0x8e, 0x9d, 0x4a, 0x2b, 0x5c,     \
                     0x01, 0x1e, 0x3f, 0x7a)

/** Where a press came from, for logging and status only. */
typedef enum {
    BLE_TRIGGER_SOURCE_GATT = 0, /**< A client wrote the trigger characteristic. */
    BLE_TRIGGER_SOURCE_HID,      /**< A connected BLE HID button reported a key. */
} ble_trigger_source_t;

/* ------------------------------------------------------------------ */
/* Stack (ble_trigger_stack.c)                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Start the controller and NimBLE host, and block until host sync.
 *
 * Idempotent. Security is configured for Just Works bonding with keys in NVS,
 * which is what an input-only button and a phone test app both negotiate.
 */
esp_err_t ble_trigger_stack_start(void);

esp_err_t ble_trigger_stack_stop(void);

bool ble_trigger_stack_is_running(void);

/** @brief Address type inferred once after sync; needed to advertise or scan. */
uint8_t ble_trigger_stack_own_addr_type(void);

/* ------------------------------------------------------------------ */
/* Trigger fan-in (cap_ble_trigger.c)                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Report a press or release from any source.
 *
 * Edge-triggered: a repeated press while already capturing is ignored rather
 * than restarting, which is what a HID button's key-repeat would otherwise do.
 */
void ble_trigger_report(ble_trigger_source_t source, bool pressed);

/** @brief Report a single push with no hold information; see BLE_TRIGGER_CMD_TAP. */
void ble_trigger_report_tap(ble_trigger_source_t source);

/* ------------------------------------------------------------------ */
/* GATT peripheral (ble_trigger_peripheral.c)                          */
/* ------------------------------------------------------------------ */

/** @brief Register the trigger service. Call before the host task starts. */
esp_err_t ble_trigger_peripheral_register(void);

esp_err_t ble_trigger_peripheral_start_advertising(void);

void ble_trigger_peripheral_stop_advertising(void);

bool ble_trigger_peripheral_is_connected(void);

/* ------------------------------------------------------------------ */
/* HID host (ble_trigger_hidh.c)                                       */
/* ------------------------------------------------------------------ */

esp_err_t ble_trigger_hidh_start(void);

void ble_trigger_hidh_stop(void);

/**
 * @brief Scan for BLE peripherals and connect to the first HID button found.
 *
 * @param duration_sec Scan window.
 * @param out_report   Optional human-readable summary of what was seen, which
 *                     is the only way to tell an unknown remote's advertising
 *                     apart from a device that is simply out of range.
 */
esp_err_t ble_trigger_hidh_scan(int duration_sec, char *out_report, size_t report_size);

bool ble_trigger_hidh_is_connected(void);

#ifdef __cplusplus
}
#endif
