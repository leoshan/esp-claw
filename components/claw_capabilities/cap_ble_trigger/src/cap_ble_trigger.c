/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_ble_trigger.h"

#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cap_voice_pipeline.h"
#include "claw_cap.h"

#include "ble_trigger_private.h"

static const char *TAG = "cap_ble_trigger";

#define BLE_TRIGGER_CAP_GROUP_ID "cap_ble_trigger"
#define BLE_TRIGGER_LOCK_TIMEOUT_MS 1000
#define BLE_TRIGGER_SCAN_REPORT_MAX 640
/* Starts an ASR task and opens a WebSocket downstream; this task only calls. */
#define BLE_TRIGGER_TASK_STACK 4096
#define BLE_TRIGGER_TASK_PRIO  5
#define BLE_TRIGGER_QUEUE_DEPTH 8
/* Long enough for Wi-Fi to associate first, so the scan is not competing with
 * the association exchange for the radio. */
#define BLE_TRIGGER_BOOT_SCAN_DELAY_MS 6000

typedef enum {
    BLE_TRIGGER_MSG_EDGE = 0, /**< A press or release that changed state. */
    BLE_TRIGGER_MSG_TAP,      /**< One push with no hold; toggle capture. */
    BLE_TRIGGER_MSG_SCAN,     /**< Look for a HID button to connect to. */
} ble_trigger_msg_kind_t;

typedef struct {
    ble_trigger_msg_kind_t kind;
    ble_trigger_source_t source;
    bool pressed;
} ble_trigger_msg_t;

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    TaskHandle_t task;
    esp_timer_handle_t hold_timer;
    esp_timer_handle_t boot_scan_timer;
    bool initialized;
    bool pressed;
    ble_trigger_source_t last_source;
    uint32_t press_count;
} ble_trigger_state_t;

static ble_trigger_state_t s_state = {0};

static const char *ble_trigger_source_name(ble_trigger_source_t source)
{
    return source == BLE_TRIGGER_SOURCE_HID ? "hid" : "gatt";
}

static esp_err_t ble_trigger_lock(void)
{
    if (!s_state.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(BLE_TRIGGER_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void ble_trigger_unlock(void)
{
    if (s_state.lock) {
        xSemaphoreGive(s_state.lock);
    }
}

/**
 * Stop a capture whose release never arrived.
 *
 * A phone app that disconnects mid-press, or a remote that walks out of range
 * while held, would otherwise leave the microphone open until the recording
 * cap expires -- and the transcript would be whatever the room said.
 */
static void ble_trigger_hold_expired(void *arg)
{
    (void)arg;

    /* Capture usually ends on its own at the recording cap well before this
     * fires, so only a still-open microphone is worth warning about. */
    if (cap_voice_pipeline_is_listening()) {
        ESP_LOGW(TAG, "No release after %d ms; stopping capture", BLE_TRIGGER_HOLD_TIMEOUT_MS);
    }
    ble_trigger_report(s_state.last_source, false);
}

static void ble_trigger_boot_scan_due(void *arg)
{
    ble_trigger_msg_t msg = {.kind = BLE_TRIGGER_MSG_SCAN};

    (void)arg;
    if (s_state.queue) {
        (void)xQueueSend(s_state.queue, &msg, 0);
    }
}

/**
 * Act on trigger edges away from the callback that produced them.
 *
 * Presses arrive on the NimBLE host task (a GATT write) and on the esp_hidh
 * event task. Starting capture from there would run an ASR task launch and a
 * mutex wait inside the BLE stack's own callback, stalling link-layer work
 * for as long as that takes. Scanning is worse: it blocks for seconds.
 */
static void ble_trigger_task(void *arg)
{
    ble_trigger_msg_t msg;

    (void)arg;

    while (xQueueReceive(s_state.queue, &msg, portMAX_DELAY) == pdTRUE) {
        esp_err_t err;

        if (msg.kind == BLE_TRIGGER_MSG_TAP) {
            bool listening = cap_voice_pipeline_is_listening();

            ESP_LOGI(TAG, "[%s] tap -> %s", ble_trigger_source_name(msg.source),
                     listening ? "stop" : "start");
            if (listening) {
                err = cap_voice_pipeline_stop_listening();
            } else {
                err = cap_voice_pipeline_start_listening();
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "tap failed: %s", esp_err_to_name(err));
            }
            /* Keep the held flag honest for anyone reading it, and make sure a
             * stale hold timer cannot stop a capture this tap just started. */
            if (ble_trigger_lock() == ESP_OK) {
                s_state.pressed = false;
                ble_trigger_unlock();
            }
            if (s_state.hold_timer) {
                (void)esp_timer_stop(s_state.hold_timer);
            }
            continue;
        }

        if (msg.kind == BLE_TRIGGER_MSG_SCAN) {
            err = ble_trigger_hidh_scan(CONFIG_CLAW_CAP_BLE_TRIGGER_HID_SCAN_SEC, NULL, 0);
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "No BLE HID button nearby; call ble_trigger_scan to retry");
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "HID scan failed: %s", esp_err_to_name(err));
            }
            continue;
        }

        if (msg.pressed) {
            if (s_state.hold_timer) {
                (void)esp_timer_stop(s_state.hold_timer);
                (void)esp_timer_start_once(s_state.hold_timer,
                                           (uint64_t)BLE_TRIGGER_HOLD_TIMEOUT_MS * 1000);
            }
            err = cap_voice_pipeline_start_listening();
            ESP_LOGI(TAG, "[%s] press -> voice_listen: %s", ble_trigger_source_name(msg.source),
                     esp_err_to_name(err));
        } else {
            if (s_state.hold_timer) {
                (void)esp_timer_stop(s_state.hold_timer);
            }
            err = cap_voice_pipeline_stop_listening();
            ESP_LOGI(TAG, "[%s] release -> voice_stop_listening: %s",
                     ble_trigger_source_name(msg.source), esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

void ble_trigger_report(ble_trigger_source_t source, bool pressed)
{
    ble_trigger_msg_t msg = {
        .kind = BLE_TRIGGER_MSG_EDGE,
        .source = source,
        .pressed = pressed,
    };
    bool changed = false;

    if (!s_state.initialized) {
        return;
    }
    if (ble_trigger_lock() != ESP_OK) {
        ESP_LOGW(TAG, "Dropping %s event: state is busy", pressed ? "press" : "release");
        return;
    }

    /* Edge-triggered, and decided here rather than in the task so a HID
     * key-repeat -- the same "held" report many times a second -- cannot fill
     * the queue. Only transitions are ever posted. */
    if (pressed != s_state.pressed) {
        s_state.pressed = pressed;
        s_state.last_source = source;
        changed = true;
        if (pressed) {
            s_state.press_count++;
        }
    }
    ble_trigger_unlock();

    if (changed && xQueueSend(s_state.queue, &msg, 0) != pdTRUE) {
        /* Rolling the state back keeps the next edge from being swallowed as
         * a repeat of one that never actually ran. */
        ESP_LOGE(TAG, "Trigger queue full; dropping %s", pressed ? "press" : "release");
        if (ble_trigger_lock() == ESP_OK) {
            s_state.pressed = !pressed;
            ble_trigger_unlock();
        }
    }
}

void ble_trigger_report_tap(ble_trigger_source_t source)
{
    ble_trigger_msg_t msg = {.kind = BLE_TRIGGER_MSG_TAP, .source = source};

    if (!s_state.initialized) {
        return;
    }

    /* A tap carries no hold duration, so it toggles: start capture if idle,
     * end it if running. Deliberately not routed through ble_trigger_report:
     * that edge-detects against the held flag, and capture ending on its own
     * at the recording cap leaves the flag set, which swallowed the next tap
     * until the hold timeout cleared it. The worker reads the pipeline
     * instead, which is the only thing that actually knows. */
    if (xQueueSend(s_state.queue, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Trigger queue full; dropping tap");
    }
}

/* ------------------------------------------------------------------ */
/* Capabilities                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t cap_ble_trigger_execute_status(const char *input_json,
                                                const claw_cap_call_context_t *ctx, char *output,
                                                size_t output_size)
{
    (void)input_json;
    (void)ctx;

    snprintf(output, output_size,
             "{\"ok\":true,\"stack\":%s,\"advertising_name\":\"%s\",\"phone_connected\":%s,"
             "\"hid_button_connected\":%s,\"listening\":%s,\"press_count\":%u}",
             ble_trigger_stack_is_running() ? "true" : "false", BLE_TRIGGER_DEVICE_NAME,
             ble_trigger_peripheral_is_connected() ? "true" : "false",
             ble_trigger_hidh_is_connected() ? "true" : "false",
             cap_voice_pipeline_is_listening() ? "true" : "false",
             (unsigned)s_state.press_count);
    return ESP_OK;
}

static esp_err_t cap_ble_trigger_execute_scan(const char *input_json,
                                              const claw_cap_call_context_t *ctx, char *output,
                                              size_t output_size)
{
    char *report;
    cJSON *root;
    cJSON *seconds_item;
    int seconds = CONFIG_CLAW_CAP_BLE_TRIGGER_HID_SCAN_SEC;
    esp_err_t err;

    (void)ctx;

    root = input_json ? cJSON_Parse(input_json) : NULL;
    seconds_item = root ? cJSON_GetObjectItem(root, "seconds") : NULL;
    if (cJSON_IsNumber(seconds_item)) {
        seconds = (int)seconds_item->valuedouble;
        if (seconds < 2 || seconds > 30) {
            seconds = CONFIG_CLAW_CAP_BLE_TRIGGER_HID_SCAN_SEC;
        }
    }
    cJSON_Delete(root);

    /* Heap, not stack: this runs on the capability dispatch task. */
    report = calloc(1, BLE_TRIGGER_SCAN_REPORT_MAX);
    if (!report) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"ESP_ERR_NO_MEM\"}");
        return ESP_ERR_NO_MEM;
    }

    /* Blocks for the scan window. Scanning shares the radio with Wi-Fi, so
     * this is deliberately on-demand rather than continuous. */
    err = ble_trigger_hidh_scan(seconds, report, BLE_TRIGGER_SCAN_REPORT_MAX);

    {
        cJSON *result = cJSON_CreateObject();
        char *rendered = NULL;

        if (result) {
            cJSON_AddBoolToObject(result, "ok", err == ESP_OK || err == ESP_ERR_NOT_FOUND);
            cJSON_AddBoolToObject(result, "hid_button_found", err == ESP_OK);
            cJSON_AddStringToObject(result, "devices", report);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
            }
            rendered = cJSON_PrintUnformatted(result);
            cJSON_Delete(result);
        }
        if (rendered) {
            snprintf(output, output_size, "%s", rendered);
            free(rendered);
        } else {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"ESP_ERR_NO_MEM\"}");
        }
    }

    free(report);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static const claw_cap_descriptor_t s_ble_trigger_descriptors[] = {
    {
        .id = "ble_trigger_status",
        .name = "ble_trigger_status",
        .family = "voice",
        .description = "Report the wireless push-to-talk state: whether BLE is up, the name "
                       "this device advertises, whether a phone or a BLE HID button is "
                       "connected, and whether the trigger is held down right now.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_ble_trigger_execute_status,
    },
    {
        .id = "ble_trigger_scan",
        .name = "ble_trigger_scan",
        .family = "voice",
        .description = "Scan for nearby BLE peripherals and connect to the first one "
                       "advertising the HID service, to pair a wireless button. Returns every "
                       "device seen with its address, signal strength and whether it is a HID "
                       "device. Blocks for the duration of the scan.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\","
            "\"minimum\":2,\"maximum\":30,\"description\":\"Scan window.\"}}}",
        .execute = cap_ble_trigger_execute_scan,
    },
};

static const claw_cap_group_t s_ble_trigger_group = {
    .group_id = BLE_TRIGGER_CAP_GROUP_ID,
    .descriptors = s_ble_trigger_descriptors,
    .descriptor_count = sizeof(s_ble_trigger_descriptors) / sizeof(s_ble_trigger_descriptors[0]),
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t cap_ble_trigger_init(void)
{
    const esp_timer_create_args_t hold_args = {
        .callback = ble_trigger_hold_expired,
        .name = "ble_trig_hold",
    };
    const esp_timer_create_args_t boot_scan_args = {
        .callback = ble_trigger_boot_scan_due,
        .name = "ble_trig_scan",
    };
    esp_err_t err;

    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing cap_ble_trigger");

    if (!s_state.lock) {
        s_state.lock = xSemaphoreCreateMutex();
    }
    if (!s_state.queue) {
        s_state.queue = xQueueCreate(BLE_TRIGGER_QUEUE_DEPTH, sizeof(ble_trigger_msg_t));
    }
    if (!s_state.lock || !s_state.queue) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (!s_state.hold_timer) {
        err = esp_timer_create(&hold_args, &s_state.hold_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot create the hold timer: %s", esp_err_to_name(err));
            goto fail;
        }
    }
    if (!s_state.task &&
        xTaskCreate(ble_trigger_task, "ble_trig", BLE_TRIGGER_TASK_STACK, NULL,
                    BLE_TRIGGER_TASK_PRIO, &s_state.task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create the trigger task");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = ble_trigger_stack_start();
    if (err != ESP_OK) {
        goto fail;
    }

    /* Set before starting any BLE activity: a client can connect and write the
     * trigger characteristic the instant advertising begins, and
     * ble_trigger_report drops events until this is true. */
    s_state.initialized = true;

#if CONFIG_CLAW_CAP_BLE_TRIGGER_ADVERTISE
    err = ble_trigger_peripheral_start_advertising();
    if (err != ESP_OK) {
        goto fail;
    }
#endif

#if CONFIG_CLAW_CAP_BLE_TRIGGER_HID_HOST
    /* A missing button is normal, so failing here is not fatal: the GATT
     * trigger still works and ble_trigger_scan can retry later. The scan runs
     * on the worker, and only after a delay -- doing it inline would hold up
     * the rest of boot for the whole scan window. */
    if (ble_trigger_hidh_start() == ESP_OK &&
        esp_timer_create(&boot_scan_args, &s_state.boot_scan_timer) == ESP_OK) {
        (void)esp_timer_start_once(s_state.boot_scan_timer,
                                   (uint64_t)BLE_TRIGGER_BOOT_SCAN_DELAY_MS * 1000);
    }
#endif

    if (!claw_cap_group_exists(s_ble_trigger_group.group_id)) {
        err = claw_cap_register_group(&s_ble_trigger_group);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot register the capability group: %s", esp_err_to_name(err));
            goto fail;
        }
    }
    /* app_claw fixes the LLM-visible list during startup, before this runs, so
     * without this the model never sees these tools. */
    claw_cap_add_llm_visible_group(s_ble_trigger_group.group_id);

    return ESP_OK;

fail:
    s_state.initialized = false;
    cap_ble_trigger_deinit();
    return err;
}

esp_err_t cap_ble_trigger_deinit(void)
{
    s_state.initialized = false;

    if (s_state.boot_scan_timer) {
        (void)esp_timer_stop(s_state.boot_scan_timer);
        (void)esp_timer_delete(s_state.boot_scan_timer);
        s_state.boot_scan_timer = NULL;
    }
    if (s_state.hold_timer) {
        (void)esp_timer_stop(s_state.hold_timer);
        (void)esp_timer_delete(s_state.hold_timer);
        s_state.hold_timer = NULL;
    }

    /* Stop the BLE sources before the worker: once the queue is gone, an
     * in-flight callback would post into freed memory. */
    ble_trigger_hidh_stop();
    ble_trigger_peripheral_stop_advertising();
    (void)ble_trigger_stack_stop();

    if (s_state.task) {
        vTaskDelete(s_state.task);
        s_state.task = NULL;
    }
    if (s_state.queue) {
        vQueueDelete(s_state.queue);
        s_state.queue = NULL;
    }

    if (claw_cap_group_exists(s_ble_trigger_group.group_id)) {
        (void)claw_cap_unregister_group(s_ble_trigger_group.group_id, 0);
    }

    if (s_state.lock) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
    }
    s_state.pressed = false;
    return ESP_OK;
}

#else /* !CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED */

esp_err_t cap_ble_trigger_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cap_ble_trigger_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED */
