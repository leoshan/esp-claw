---
{
  "name": "voice_push_to_talk",
  "description": "Hold a button to talk to the device: press to start capturing, release to stop and transcribe. Covers the on-board BOOT button, wiring an external button, and how the background script is started and stopped.",
  "metadata": {
    "cap_groups": [
      "cap_lua",
      "cap_voice_pipeline"
    ],
    "manage_mode": "readonly"
  }
}
---

# Voice push-to-talk

Binds one or more buttons (active low) to the voice pipeline:

- **press** calls `voice_listen`, which opens the microphone and starts
  streaming audio to the recognizer
- **release** calls `voice_stop_listening`, which ends the utterance

Holding the button is what bounds the recording, so a sentence is never cut off
by the `CLAW_CAP_VOICE_RECORD_DURATION_SEC` timeout unless the speaker runs past
it. The recognized text is published as a normal user message, so the agent
answers it and the reply is spoken through the speaker.

## Buttons

| GPIO | Button |
| --- | --- |
| 0 | on-board BOOT, small and awkward to hold |
| 21 | external button, wired between the pin and GND |

Any button is enough on its own; a pin that fails to open is skipped with a log
line rather than taking the watcher down with it.

### Wiring an external button

Two wires, no other parts: one side of the switch to the GPIO, the other to any
GND. The driver turns on the internal pull-up for an active-low button, so no
resistor is needed.

GPIO 21 is free on this board and safe to hold at boot. Avoid the strapping
pins (0 is already handled, plus 3, 45, 46), the flash (26-32), the octal PSRAM
(33-37), USB (19, 20), UART0 (43, 44), and whatever the board's own peripherals
claim -- here the audio pins (4, 5, 6, 7, 15, 16), I2C (41, 42) and the LED
strip (38). GPIO 1, 2, 8-14, 17, 18, 39, 40 and 47 are the other free choices.

### Moving the button

The pin list comes from the `startup_voice_push_to_talk` router rule, so
changing it needs no rebuild -- edit the DATA copy of `router_rules.json`:

```json
"args": {"gpios": [0, 21]}
```

A comma-separated string such as `"0,21"` works too.

## Starting it

The script loops forever polling the button, so it must run asynchronously:

```json
{"path":"{CUR_SKILL_DIR}/scripts/push_to_talk.lua","timeout_ms":0,"name":"voice_ptt","exclusive":"voice_ptt","args":{"gpios":[0,21]}}
```

Call `lua_run_script_async` with that payload. The `startup_voice_push_to_talk`
router rule already does this on `startup/boot_completed`.

## Stopping it

Call `lua_stop_async_job` with `{"name":"voice_ptt"}`. Do this before running any
other script that needs one of the watched pins.

## Requirements

- A board that declares `i2s_audio_in` and `i2s_audio_out`, and firmware built
  with `CLAW_CAP_VOICE_PIPELINE_ENABLED`
- ASR and TTS credentials configured on the Voice settings page
