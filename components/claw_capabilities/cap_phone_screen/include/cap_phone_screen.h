/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phone screen capability: an AirPlay screen-mirroring receiver (ported from UxPlay via
 * esp32-iphone-agent) that forwards the phone's H.264 stream to a relay on the LAN, which
 * decodes it and serves the latest frame as JPEG. Links GPL-3.0 code: firmware built with
 * this component enabled must be distributed under the GPL.
 */
#pragma once

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the running claw_core (NULL before it is up); used when a caller's context has none. */
typedef claw_core_handle_t (*cap_phone_screen_core_getter_t)(void);

/**
 * @brief Register the phone screen capability group and start the AirPlay receiver
 *        once the station interface has an IP address.
 *
 * @param get_core  Fallback core getter for phone_look calls that arrive without a core in their
 *                  context (e.g. from Lua); may be NULL.
 */
esp_err_t cap_phone_screen_register_group(cap_phone_screen_core_getter_t get_core);

#ifdef __cplusplus
}
#endif
