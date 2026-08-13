# cap_ble_trigger

Wireless push-to-talk. A remote BLE press starts voice capture and the matching
release stops it, exactly as the on-board BOOT button does through
`voice_push_to_talk`.

Two sources feed the same trigger and both can run at once:

| Source | Direction | Use |
| --- | --- | --- |
| Trigger GATT service | this device is the **peripheral**, the phone connects in | Testing, and using a phone as the button |
| BLE HID button (HOGP) | this device is the **central**, it connects out | A physical remote |

Both call `ble_trigger_report()`, which edge-detects and hands the transition to
a worker task. Nothing touches the voice pipeline from a BLE callback.

## Testing with a phone BLE app

No hardware button needed. Any BLE debugging app (nRF Connect, LightBlue, BLE
调试助手) works.

1. Flash and boot. The log shows:
   ```
   I ble_trig_stack: NimBLE up as 'esp-claw-mic', own address type 0
   I ble_trig_periph: Advertising as 'esp-claw-mic'; write 0x01/0x00 to characteristic 7a3f1e01-...
   ```
2. Scan in the phone app and connect to **`esp-claw-mic`**. No pairing needed —
   the trigger characteristic deliberately carries no encryption requirement so
   a debugging app can drive it directly.
3. Find service `7a3f1e00-5c2b-4a9d-8e11-2f6b0c9d4a71`, characteristic
   `7a3f1e01-5c2b-4a9d-8e11-2f6b0c9d4a71`.
4. Write one byte:

   | Value | Meaning |
   | --- | --- |
   | `0x01` | press — starts capture, hold while you speak |
   | `0x00` | release — stops capture and transcribes |
   | `0x02` | tap — toggles: starts if idle, stops if capturing |

   Write `0x01`, say something, then write `0x00`. Reading the characteristic
   returns `0x01` while capture is running.

Use `0x02` if the app makes two writes awkward — one tap starts, the next stops.

A write must be exactly one byte. Some apps default to sending text; switch the
input format to hex/byte first, or the write is rejected with
`BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN`.

## Connecting a real HID button

The button must be **BLE** (ESP32-S3 has no Bluetooth Classic at all) and must
advertise the HID service `0x1812`.

A scan runs once, six seconds after boot. To retry:

```
cap call ble_trigger_scan "{\"seconds\":10}"
```

which lists everything it saw, so an unknown remote can be told apart from one
that is simply out of range:

```json
{"ok":true,"hid_button_found":true,"devices":"seen 3 device(s)\n  ff:ee:...  rssi=-52 hid=yes name=Shutter"}
```

`cap call ble_trigger_status "{}"` reports what is connected.

### Press and release

`ble_trigger_report_is_press()` treats any non-zero input report as held and an
all-zero report as released, without decoding keycodes — the report layout is
only knowable from the device's own report map, and every relevant layout (boot
keyboard, consumer control, vendor one-button) idles at all-zero.

Remotes that emit a single tap rather than a hold cannot do hold-to-talk. Test a
candidate before buying: pair it to a computer, open a text editor and hold the
button. Repeating characters mean it reports press and release separately.

## Constraints

- **Owns the NimBLE stack.** The `ble` and `ble_hid` Lua modules each own it
  too, so only one of the three may run.
- **Requires the central and observer roles**, which
  `application/edge_agent/sdkconfig.defaults` turns off. The voice board
  re-enables them along with `CONFIG_BT_NIMBLE_HID_SERVICE`, without which
  esp_hid's `nimble_hidh.c` compiles to nothing.
- **Scanning shares the radio with Wi-Fi**, so it is on-demand only, passive,
  and at 50% duty. It never runs on a timer.
- A press with no release stops capture after
  `CLAW_CAP_BLE_TRIGGER_HOLD_TIMEOUT_MS` (15 s default), so a link that drops
  mid-press cannot leave the microphone open.
