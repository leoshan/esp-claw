/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mirror task -> PSRAM stream buffer -> sender task -> TCP -> relay (raw Annex-B H.264).
 * The phone sends an IDR only when mirroring starts (none for minutes afterwards), so the
 * relay must keep one decoder running for the whole session. On a (re)connect the sender
 * replays SPS/PPS and the latest IDR so the relay always has a decodable starting point.
 */
#include "phone_stream.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "phone_stream";

/* H.264 access unit delimiter, sent after every picture. The relay feeds a raw Annex-B stream to
 * ffmpeg, whose parser only knows a picture is complete when the next one starts; the phone sends
 * pictures only when the screen changes, so the last picture of a transition sat in the parser
 * until the next change (measured 2026-10-25: the board had a new picture 0.2-1.1 s after a click,
 * the relay showed it 4-14 s later). A delimiter starts the next unit at once. */
static const uint8_t PHONE_STREAM_AUD[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};

#define PHONE_STREAM_QUEUE_BYTES   (1024 * 1024)
#define PHONE_STREAM_CONFIG_MAX    256
#define PHONE_STREAM_IDR_MAX       (512 * 1024)
#define PHONE_STREAM_CHUNK_MAX     (256 * 1024)
#define PHONE_STREAM_RETRY_MS      2000
#define PHONE_STREAM_TASK_STACK    4096
#define PHONE_STREAM_TASK_PRIO     4

typedef struct {
    uint32_t len;
} phone_stream_rec_t;

static struct {
    StreamBufferHandle_t queue;
    StaticStreamBuffer_t queue_ctrl;
    uint8_t *queue_mem;
    uint8_t *chunk;
    SemaphoreHandle_t lock;          /* guards config/idr copies and stats */
    uint8_t config[PHONE_STREAM_CONFIG_MAX];
    size_t config_len;
    uint8_t *idr;
    size_t idr_len;
    char host[64];
    uint16_t port;
    phone_stream_stats_t stats;
} s_ps;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int k = send(fd, p, n, 0);
        if (k <= 0) {
            return false;
        }
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static int relay_connect(void)
{
    char port[8];
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int fd = -1;

    snprintf(port, sizeof(port), "%u", (unsigned)s_ps.port);
    if (getaddrinfo(s_ps.host, port, &hints, &res) != 0 || !res) {
        return -1;
    }
    fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

/* SPS/PPS + latest IDR, so a relay that (re)connects mid-session can start decoding. */
static bool send_anchor(int fd)
{
    bool ok = true;

    xSemaphoreTake(s_ps.lock, portMAX_DELAY);
    if (s_ps.config_len) {
        ok = send_all(fd, s_ps.config, s_ps.config_len);
    }
    if (ok && s_ps.idr_len) {
        ok = send_all(fd, s_ps.idr, s_ps.idr_len) && send_all(fd, PHONE_STREAM_AUD, sizeof(PHONE_STREAM_AUD));
    }
    xSemaphoreGive(s_ps.lock);
    return ok;
}

static bool recv_exact(uint8_t *out, size_t n)
{
    size_t got = 0;

    while (got < n) {
        got += xStreamBufferReceive(s_ps.queue, out + got, n - got, pdMS_TO_TICKS(500));
        if (got == 0) {
            return false;
        }
    }
    return true;
}

static void sender_task(void *arg)
{
    int fd = -1;
    (void)arg;

    for (;;) {
        if (fd < 0) {
            fd = relay_connect();
            if (fd < 0 || !send_anchor(fd)) {
                if (fd >= 0) {
                    close(fd);
                    fd = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(PHONE_STREAM_RETRY_MS));
                continue;
            }
            ESP_LOGI(TAG, "relay connected %s:%u", s_ps.host, (unsigned)s_ps.port);
            xSemaphoreTake(s_ps.lock, portMAX_DELAY);
            s_ps.stats.relay_connected = true;
            xSemaphoreGive(s_ps.lock);
        }

        phone_stream_rec_t rec;
        if (!recv_exact((uint8_t *)&rec, sizeof(rec))) {
            continue;
        }
        /* a record is always written whole by the producer, so the payload follows */
        while (!recv_exact(s_ps.chunk, rec.len)) {
        }
        if (!send_all(fd, s_ps.chunk, rec.len)) {
            ESP_LOGW(TAG, "relay link lost");
            close(fd);
            fd = -1;
            xSemaphoreTake(s_ps.lock, portMAX_DELAY);
            s_ps.stats.relay_connected = false;
            xSemaphoreGive(s_ps.lock);
            continue;
        }
        xSemaphoreTake(s_ps.lock, portMAX_DELAY);
        s_ps.stats.bytes_sent += rec.len;
        xSemaphoreGive(s_ps.lock);
    }
}

static void enqueue(const uint8_t *p, size_t len)
{
    phone_stream_rec_t rec = {.len = (uint32_t)len};
    bool ok = len <= PHONE_STREAM_CHUNK_MAX &&
              xStreamBufferSpacesAvailable(s_ps.queue) >= sizeof(rec) + len;

    if (ok) {
        xStreamBufferSend(s_ps.queue, &rec, sizeof(rec), 0);
        xStreamBufferSend(s_ps.queue, p, len, 0);
    }
    if (!ok) {
        xSemaphoreTake(s_ps.lock, portMAX_DELAY);
        s_ps.stats.dropped++;
        xSemaphoreGive(s_ps.lock);
    }
}

void phone_stream_on_config(const uint8_t *sps_pps, size_t len)
{
    if (!s_ps.queue || len > PHONE_STREAM_CONFIG_MAX) {
        return;
    }
    xSemaphoreTake(s_ps.lock, portMAX_DELAY);
    memcpy(s_ps.config, sps_pps, len);
    s_ps.config_len = len;
    s_ps.idr_len = 0;  /* a new session: the old IDR no longer matches */
    xSemaphoreGive(s_ps.lock);
    enqueue(sps_pps, len);
}

void phone_stream_on_au(const uint8_t *au, size_t len, bool idr)
{
    if (!s_ps.queue) {
        return;
    }
    xSemaphoreTake(s_ps.lock, portMAX_DELAY);
    s_ps.stats.au_count++;
    s_ps.stats.last_au_ms = now_ms();
    if (idr) {
        s_ps.stats.idr_count++;
        if (len <= PHONE_STREAM_IDR_MAX) {
            memcpy(s_ps.idr, au, len);
            s_ps.idr_len = len;
        }
    }
    xSemaphoreGive(s_ps.lock);
    enqueue(au, len);
    enqueue(PHONE_STREAM_AUD, sizeof(PHONE_STREAM_AUD));
}

void phone_stream_get_stats(phone_stream_stats_t *out)
{
    if (!s_ps.lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_ps.lock, portMAX_DELAY);
    *out = s_ps.stats;
    xSemaphoreGive(s_ps.lock);
}

esp_err_t phone_stream_start(const char *relay_host, uint16_t relay_port)
{
    if (s_ps.queue) {
        return ESP_OK;
    }
    if (!relay_host || !relay_host[0]) {
        ESP_LOGW(TAG, "no relay host configured; the stream is not forwarded");
        return ESP_ERR_INVALID_ARG;
    }
    s_ps.queue_mem = heap_caps_malloc(PHONE_STREAM_QUEUE_BYTES + 1, MALLOC_CAP_SPIRAM);
    s_ps.chunk = heap_caps_malloc(PHONE_STREAM_CHUNK_MAX, MALLOC_CAP_SPIRAM);
    s_ps.idr = heap_caps_malloc(PHONE_STREAM_IDR_MAX, MALLOC_CAP_SPIRAM);
    s_ps.lock = xSemaphoreCreateMutex();
    if (!s_ps.queue_mem || !s_ps.chunk || !s_ps.idr || !s_ps.lock) {
        goto fail;
    }
    strlcpy(s_ps.host, relay_host, sizeof(s_ps.host));
    s_ps.port = relay_port;
    s_ps.queue = xStreamBufferCreateStatic(PHONE_STREAM_QUEUE_BYTES, 1, s_ps.queue_mem, &s_ps.queue_ctrl);
    if (!s_ps.queue) {
        goto fail;
    }
    if (xTaskCreatePinnedToCore(sender_task, "phone_stream", PHONE_STREAM_TASK_STACK, NULL,
                                PHONE_STREAM_TASK_PRIO, NULL, 1) != pdPASS) {
        goto fail;
    }
    ESP_LOGI(TAG, "forwarding to relay %s:%u", s_ps.host, (unsigned)s_ps.port);
    return ESP_OK;

fail:
    s_ps.queue = NULL;
    heap_caps_free(s_ps.queue_mem);
    heap_caps_free(s_ps.chunk);
    heap_caps_free(s_ps.idr);
    if (s_ps.lock) {
        vSemaphoreDelete(s_ps.lock);
    }
    memset(&s_ps, 0, sizeof(s_ps));
    return ESP_ERR_NO_MEM;
}
