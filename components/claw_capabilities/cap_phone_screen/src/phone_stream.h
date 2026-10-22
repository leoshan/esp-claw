/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Forwards the mirrored H.264 stream (Annex-B) to the relay over one TCP connection.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t au_count;       /**< access units received from the phone */
    uint32_t idr_count;      /**< IDR access units received */
    uint32_t dropped;        /**< access units dropped because the relay link was slow or down */
    uint32_t bytes_sent;     /**< bytes written to the relay */
    bool relay_connected;
    int64_t last_au_ms;      /**< board time of the last access unit, 0 if none */
} phone_stream_stats_t;

/** Allocate the PSRAM queue and start the sender task (relay host from Kconfig). */
esp_err_t phone_stream_start(const char *relay_host, uint16_t relay_port);

/** AirPlay callbacks: SPS+PPS and access units, both Annex-B; data valid only during the call. */
void phone_stream_on_config(const uint8_t *sps_pps, size_t len);
void phone_stream_on_au(const uint8_t *au, size_t len, bool idr);

void phone_stream_get_stats(phone_stream_stats_t *out);
