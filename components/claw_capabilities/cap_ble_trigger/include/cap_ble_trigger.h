/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up wireless push-to-talk over BLE.
 *
 * Starts the NimBLE stack, advertises the trigger GATT service so a phone can
 * act as the button, and (when enabled) scans for a BLE HID button to connect
 * out to. Both sources drive the same voice capture as the on-board button.
 *
 * This owns the NimBLE stack for the lifetime of the process. The `ble` and
 * `ble_hid` Lua modules each own it too, so only one of the three may run.
 *
 * @return ESP_ERR_NOT_SUPPORTED when NimBLE lacks the roles this needs.
 */
esp_err_t cap_ble_trigger_init(void);

/** @brief Stop advertising, drop connections and release the NimBLE stack. */
esp_err_t cap_ble_trigger_deinit(void);

#ifdef __cplusplus
}
#endif
