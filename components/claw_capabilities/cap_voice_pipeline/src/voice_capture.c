/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "voice_pipeline_private.h"

static const char *TAG = "voice_capture";

/* Read in short chunks so the gap between one read returning and the next
 * starting stays negligible next to what the I2S DMA holds. */
#define CAPTURE_CHUNK_MS    100
#define CAPTURE_CHUNK_BYTES (VOICE_ASR_SAMPLE_RATE * 2 * CAPTURE_CHUNK_MS / 1000)
#define CAPTURE_READ_TIMEOUT_MS 300
/* Above the ASR task, which is what this exists to decouple from: the whole
 * point is that a slow WebSocket write cannot delay a microphone read. */
#define CAPTURE_TASK_PRIO  6
#define CAPTURE_TASK_STACK 3072

static RingbufHandle_t s_ring = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile size_t s_dropped_bytes = 0;

/**
 * Drain the microphone into the ring for as long as capture is enabled.
 *
 * This task exists because the I2S RX DMA holds only about 90 ms (6 descriptors
 * x 240 frames at 16 kHz). Reading the microphone on the same task that writes
 * to the WebSocket meant every TLS write over ~90 ms silently overwrote
 * captured audio, which is heard as words dropped from the middle of an
 * utterance. Here the read loop never waits on the network.
 */
static void voice_capture_task(void *arg)
{
    uint8_t *chunk = malloc(CAPTURE_CHUNK_BYTES);

    (void)arg;

    if (!chunk) {
        ESP_LOGE(TAG, "Cannot allocate the capture chunk");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        size_t bytes_read = 0;
        esp_err_t err = voice_audio_io_read(chunk, CAPTURE_CHUNK_BYTES, &bytes_read,
                                            CAPTURE_READ_TIMEOUT_MS);

        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }
        /* Never block: stalling here would stop draining the DMA, which is the
         * exact failure this task exists to prevent. A full ring means the
         * consumer has fallen behind, and dropping the newest audio is the
         * lesser evil -- it is at least reported. */
        if (xRingbufferSend(s_ring, chunk, bytes_read, 0) != pdTRUE) {
            s_dropped_bytes += bytes_read;
        }
    }

    free(chunk);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_capture_start(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ring) {
        /* PSRAM: several seconds of audio is far too much for internal RAM,
         * and only a normal task touches it, so no DMA capability is needed. */
        s_ring = xRingbufferCreateWithCaps(VOICE_CAPTURE_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            s_ring = xRingbufferCreate(VOICE_CAPTURE_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
        }
        if (!s_ring) {
            ESP_LOGE(TAG, "Cannot allocate the %d byte capture ring", VOICE_CAPTURE_RING_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }

    voice_capture_flush();
    s_dropped_bytes = 0;
    s_running = true;

    if (xTaskCreate(voice_capture_task, "v_cap", CAPTURE_TASK_STACK, NULL, CAPTURE_TASK_PRIO,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create the capture task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_capture_stop(void)
{
    s_running = false;

    /* The task can be inside a read for up to the read timeout. Waiting for it
     * to clear keeps a restart from running two readers on one channel. */
    for (int waited = 0; s_task && waited < CAPTURE_READ_TIMEOUT_MS * 2; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task) {
        ESP_LOGW(TAG, "Capture task did not exit in time");
    }
    if (s_dropped_bytes) {
        ESP_LOGW(TAG, "Dropped %u ms of audio: the consumer could not keep up",
                 (unsigned)(s_dropped_bytes * 1000 / (VOICE_ASR_SAMPLE_RATE * 2)));
    }
}

size_t voice_capture_read(void *dest, size_t max_bytes, uint32_t timeout_ms)
{
    size_t copied = 0;

    if (!s_ring || !dest || max_bytes == 0) {
        return 0;
    }

    /* ReceiveUpTo hands back a pointer into the ring, which may wrap, so the
     * caller's buffer is filled across at most two of these. */
    while (copied < max_bytes) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &item_size,
                                            copied == 0 ? pdMS_TO_TICKS(timeout_ms) : 0,
                                            max_bytes - copied);

        if (!item) {
            break;
        }
        memcpy((uint8_t *)dest + copied, item, item_size);
        vRingbufferReturnItem(s_ring, item);
        copied += item_size;
    }
    return copied;
}

void voice_capture_flush(void)
{
    size_t item_size = 0;
    void *item;

    if (!s_ring) {
        return;
    }
    while ((item = xRingbufferReceiveUpTo(s_ring, &item_size, 0, VOICE_CAPTURE_RING_BYTES)) !=
           NULL) {
        vRingbufferReturnItem(s_ring, item);
    }
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
