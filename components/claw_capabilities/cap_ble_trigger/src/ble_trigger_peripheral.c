/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

#include "ble_trigger_private.h"
#include "cap_voice_pipeline.h"

static const char *TAG = "ble_trig_periph";

static const ble_uuid128_t s_svc_uuid = BLE_TRIGGER_SVC_UUID;
static const ble_uuid128_t s_chr_uuid = BLE_TRIGGER_CHR_UUID;

static uint16_t s_trigger_val_handle = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_advertising = false;
static bool s_registered = false;

static int ble_trigger_gap_event(struct ble_gap_event *event, void *arg);

/**
 * Handle a write to the trigger characteristic.
 *
 * One byte, so a phone BLE tool can send it without any encoding: 0x01 press,
 * 0x00 release, 0x02 a single tap for clients that cannot hold. Reads report
 * whether capture is currently running.
 */
static int ble_trigger_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t command = 0;
    uint16_t len = 0;
    int rc;

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        rc = ble_hs_mbuf_to_flat(ctxt->om, &command, sizeof(command), &len);
        if (rc != 0 || len != sizeof(command)) {
            ESP_LOGW(TAG, "Trigger write must be exactly 1 byte (got %u)", len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        switch (command) {
        case BLE_TRIGGER_CMD_PRESS:
            ble_trigger_report(BLE_TRIGGER_SOURCE_GATT, true);
            break;
        case BLE_TRIGGER_CMD_RELEASE:
            ble_trigger_report(BLE_TRIGGER_SOURCE_GATT, false);
            break;
        case BLE_TRIGGER_CMD_TAP:
            ble_trigger_report_tap(BLE_TRIGGER_SOURCE_GATT);
            break;
        default:
            ESP_LOGW(TAG, "Unknown trigger command 0x%02x", command);
            return BLE_ATT_ERR_UNLIKELY;
        }
        return 0;

    case BLE_GATT_ACCESS_OP_READ_CHR:
        command = cap_voice_pipeline_is_listening() ? BLE_TRIGGER_CMD_PRESS
                                                    : BLE_TRIGGER_CMD_RELEASE;
        rc = os_mbuf_append(ctxt->om, &command, sizeof(command));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_chr_def s_trigger_chrs[] = {
    {
        .uuid = &s_chr_uuid.u,
        .access_cb = ble_trigger_chr_access,
        /* Write-without-response as well as write: a button wants the lowest
         * possible latency, and the client has nothing useful to do with the
         * acknowledgement. No encryption flags, so a debugging app can drive
         * this without pairing first. */
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_trigger_val_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def s_trigger_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_trigger_chrs,
    },
    {0},
};

esp_err_t ble_trigger_peripheral_register(void)
{
    int rc;

    if (s_registered) {
        return ESP_OK;
    }

    rc = ble_gatts_count_cfg(s_trigger_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_trigger_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: rc=%d", rc);
        return ESP_FAIL;
    }

    s_registered = true;
    return ESP_OK;
}

esp_err_t ble_trigger_peripheral_start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    const char *name;
    int rc;

    if (s_advertising) {
        return ESP_OK;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* The 128-bit service UUID alone would fill the 31-byte payload and leave
     * no room for the name, and the name is what the user picks out of a scan
     * list. Clients find the service by connecting and discovering instead. */
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return ESP_FAIL;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(ble_trigger_stack_own_addr_type(), NULL, BLE_HS_FOREVER, &adv_params,
                           ble_trigger_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
        return ESP_FAIL;
    }

    s_advertising = true;
    ESP_LOGI(TAG, "Advertising as '%s'; write 0x01/0x00 to characteristic %s", name,
             "7a3f1e01-5c2b-4a9d-8e11-2f6b0c9d4a71");
    return ESP_OK;
}

void ble_trigger_peripheral_stop_advertising(void)
{
    if (s_advertising) {
        (void)ble_gap_adv_stop();
        s_advertising = false;
    }
}

bool ble_trigger_peripheral_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

/**
 * Answer a pairing request.
 *
 * NimBLE raises this and then waits for ble_sm_inject_io(); an unanswered
 * request stalls the pairing until the peer gives up and drops the link with
 * HCI 0x13 "remote user terminated", which reads on the phone as pairing
 * simply failing.
 *
 * Nothing here needs an encrypted link -- the trigger characteristic carries
 * no security flags -- so this accepts whatever the peer asks for rather than
 * defending a secret. The action is logged because which one is negotiated
 * depends on the phone, and that is otherwise invisible.
 */
static int ble_trigger_handle_passkey(struct ble_gap_passkey_params *params, uint16_t conn_handle)
{
    struct ble_sm_io io = {.action = params->action};
    int rc;

    switch (params->action) {
    case BLE_SM_IOACT_NUMCMP:
        /* Both sides show a number for the user to compare. This side has no
         * display, and MITM protection is off, so confirming is all that is
         * left; it is equivalent to Just Works. */
        io.numcmp_accept = 1;
        ESP_LOGI(TAG, "Pairing: confirming numeric comparison %06" PRIu32, params->numcmp);
        break;

    case BLE_SM_IOACT_DISP:
        /* The peer wants a passkey this side is supposed to display. There is
         * no screen, so the serial log is the display. */
        io.passkey = params->numcmp;
        ESP_LOGW(TAG, "Pairing: enter passkey %06" PRIu32 " on the phone", io.passkey);
        break;

    case BLE_SM_IOACT_NONE:
        return 0;

    default:
        /* INPUT would need the passkey the phone shows typed in here, and OOB
         * needs an out-of-band channel. Neither exists on this device. */
        ESP_LOGE(TAG,
                 "Pairing needs action %d, which this device cannot do. Connect without "
                 "pairing instead: the trigger characteristic needs no encryption.",
                 params->action);
        return BLE_ATT_ERR_UNLIKELY;
    }

    rc = ble_sm_inject_io(conn_handle, &io);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_sm_inject_io failed: rc=%d", rc);
    }
    return rc;
}

static int ble_trigger_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
            ESP_LOGI(TAG, "Client connected (handle %u)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connect failed: status=%d", event->connect.status);
            s_advertising = false;
            (void)ble_trigger_peripheral_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected: reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* A link that drops mid-press must not leave the microphone open. */
        ble_trigger_report(BLE_TRIGGER_SOURCE_GATT, false);
        s_advertising = false;
        (void)ble_trigger_peripheral_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        (void)ble_trigger_peripheral_start_advertising();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        return ble_trigger_handle_passkey(&event->passkey.params, event->passkey.conn_handle);

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;

        /* The peer is pairing again while this side still holds a bond for it,
         * which is what happens after the phone forgets the device or a
         * previous attempt half-completed. Keeping the stale keys makes every
         * later attempt fail; dropping them lets the retry succeed. */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ESP_LOGW(TAG, "Peer is re-pairing; dropping the stale bond");
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Link encrypted");
        } else {
            ESP_LOGW(TAG, "Pairing failed: status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
        return 0;

    default:
        return 0;
    }
}

#endif /* CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED */
