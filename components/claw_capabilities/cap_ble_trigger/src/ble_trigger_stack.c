/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED

#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_trigger_private.h"

static const char *TAG = "ble_trig_stack";

/* The host signals sync from its own task; everything else waits on this. */
#define BLE_TRIGGER_SYNC_TIMEOUT_MS 5000
#define BLE_TRIGGER_EXIT_TIMEOUT_MS 2000

static SemaphoreHandle_t s_sync_sem = NULL;
static SemaphoreHandle_t s_exit_sem = NULL;
static bool s_controller_started = false;
static bool s_nimble_inited = false;
static bool s_host_task_started = false;
static bool s_running = false;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static void ble_trigger_on_reset(int reason)
{
    /* The controller restarts the host by itself; on_sync runs again after. */
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void ble_trigger_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);

    if (rc != 0) {
        ESP_LOGE(TAG, "No usable BLE address: rc=%d", rc);
    } else {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "Cannot infer own address type: rc=%d", rc);
        }
    }

    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void ble_trigger_host_task(void *param)
{
    (void)param;

    /* Returns only once nimble_port_stop() has run. */
    nimble_port_run();
    nimble_port_freertos_deinit();

    if (s_exit_sem) {
        xSemaphoreGive(s_exit_sem);
    }
    vTaskDelete(NULL);
}

static esp_err_t ble_trigger_controller_start(void)
{
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    esp_err_t err;

    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return ESP_OK;
    }
    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

        err = esp_bt_controller_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Controller init failed: %s", esp_err_to_name(err));
            return err;
        }
    } else if (status != ESP_BT_CONTROLLER_STATUS_INITED) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_controller_started = true;
    return ESP_OK;
}

/**
 * Just Works bonding with the keys kept in NVS.
 *
 * A button has no display and no keypad, so it can only ever negotiate
 * BLE_HS_IO_NO_INPUT_OUTPUT, which rules out MITM protection. Bonding still
 * matters: HID peripherals refuse to hand out input reports over an
 * unencrypted link, and persisting the keys is what lets the button reconnect
 * silently after it sleeps.
 */
static void ble_trigger_configure_security(void)
{
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

esp_err_t ble_trigger_stack_start(void)
{
    esp_err_t err;

    if (s_running) {
        return ESP_OK;
    }

    if (!s_sync_sem) {
        s_sync_sem = xSemaphoreCreateBinary();
    }
    if (!s_exit_sem) {
        s_exit_sem = xSemaphoreCreateBinary();
    }
    if (!s_sync_sem || !s_exit_sem) {
        return ESP_ERR_NO_MEM;
    }

    err = ble_trigger_controller_start();
    if (err != ESP_OK) {
        goto fail;
    }

    if (!s_nimble_inited) {
        err = esp_nimble_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
            goto fail;
        }
        s_nimble_inited = true;
    }

    ble_hs_cfg.reset_cb = ble_trigger_on_reset;
    ble_hs_cfg.sync_cb = ble_trigger_on_sync;
    ble_trigger_configure_security();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Services must be registered before the host starts: NimBLE builds the
     * attribute table once at startup and rejects additions afterwards. */
    err = ble_trigger_peripheral_register();
    if (err != ESP_OK) {
        goto fail;
    }

    if (ble_svc_gap_device_name_set(BLE_TRIGGER_DEVICE_NAME) != 0) {
        ESP_LOGE(TAG, "Cannot set device name");
        err = ESP_FAIL;
        goto fail;
    }

    nimble_port_freertos_init(ble_trigger_host_task);
    s_host_task_started = true;

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(BLE_TRIGGER_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE host did not sync within %d ms", BLE_TRIGGER_SYNC_TIMEOUT_MS);
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }

    s_running = true;
    ESP_LOGI(TAG, "NimBLE up as '%s', own address type %u", BLE_TRIGGER_DEVICE_NAME,
             s_own_addr_type);
    return ESP_OK;

fail:
    ble_trigger_stack_stop();
    return err;
}

esp_err_t ble_trigger_stack_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (s_host_task_started) {
        esp_err_t err;

        /* Clear any stale give so the wait below reflects this stop. */
        if (s_exit_sem) {
            (void)xSemaphoreTake(s_exit_sem, 0);
        }
        err = nimble_port_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nimble_port_stop: %s", esp_err_to_name(err));
            ret = err;
        } else if (s_exit_sem &&
                   xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(BLE_TRIGGER_EXIT_TIMEOUT_MS)) !=
                       pdTRUE) {
            ESP_LOGW(TAG, "Host task did not exit; leaving the stack up");
            return ESP_ERR_TIMEOUT;
        }
        s_host_task_started = false;
    }

    if (s_nimble_inited) {
        esp_nimble_deinit();
        s_nimble_inited = false;
    }
    if (s_controller_started) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_controller_started = false;
    }

    s_running = false;
    return ret;
}

bool ble_trigger_stack_is_running(void)
{
    return s_running;
}

uint8_t ble_trigger_stack_own_addr_type(void)
{
    return s_own_addr_type;
}

#endif /* CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED */
