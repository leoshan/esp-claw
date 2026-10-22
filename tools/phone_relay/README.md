# Phone Screen Relay

LAN relay for `cap_phone_screen` / the `phone_agent` skill. The board receives the iPhone's AirPlay
screen mirroring but cannot decode H.264, so it forwards the raw stream here; the relay keeps one
ffmpeg decoder running and serves the latest frame, plus the pixel-difference queries the agent
uses to find the pointer and to tell when the screen has settled.

```
iPhone --AirPlay--> board --raw H.264 (TCP 8091)--> relay (ffmpeg)
board <--HTTP 8090: /frame.jpg, /mark, /diff, /still-- relay
```

## Dependency

Python 3 standard library and an `ffmpeg` binary on `PATH`.

## Usage

```bash
python3 -I -u tools/phone_relay/relay.py [--width 540] [--stream-port 8091] [--http-port 8090]
```

Set the board's `CONFIG_CAP_PHONE_SCREEN_RELAY_HOST` to this machine's LAN IP (the ports default to
8091 / 8090 on both sides).

The phone only sends a key frame when mirroring starts, so after restarting the relay (or the board)
start Screen Mirroring again on the phone. Until then, and whenever stream data arrives without a
decodable frame, every endpoint refuses with "decoder stalled" instead of serving a stale screen.

## HTTP endpoints

| Endpoint | Returns |
|---|---|
| `GET /frame.jpg` | latest frame as JPEG (`?grid=1`: with a 0-1000 coordinate grid) |
| `GET /status` | stream and decoder counters |
| `GET /mark?slot=` | remembers the current small grey frame |
| `GET /diff?slot=&near=x,y` | where the screen changed since `/mark` (bbox, center, 0-1000 units); `near` limits the search to around a point |
| `GET /diff?...&pointer=1` | with `near`: the region that looks like the pointer nudged by +x +y (two opposite-sign lobes) and its resting position `rest` |
| `GET /still` | `since_ms`: how long the screen below the status bar has not changed |

## Tests (`tests/`)

Lua scripts for the board, uploaded to the DATA partition and run with `lua --run-async`:

- `click_timing_test.lua`: whether BLE HID clicks reach iOS as taps or long presses. Run it only on a
  harmless page (Settings > Accessibility > Touch > Haptic Touch).
- `phone_tap_test.lua`: open-loop and wiggle-before-click taps on Home Screen icons. **It swipes and
  taps on the Home Screen**; use it only on a test phone layout.
