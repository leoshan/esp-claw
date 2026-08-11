---
{
  "name": "voice_push_to_talk",
  "description": "Hold the on-board BOOT button to talk to the device: press to start capturing, release to stop and transcribe. Explains how the push-to-talk background script is started and stopped.",
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

Binds the on-board BOOT button (GPIO 0, active low) to the voice pipeline:

- **press** calls `voice_listen`, which opens the microphone and starts
  streaming audio to the recognizer
- **release** calls `voice_stop_listening`, which ends the utterance

Holding the button is what bounds the recording, so a sentence is never cut off
by the `CLAW_CAP_VOICE_RECORD_DURATION_SEC` timeout unless the speaker runs past
it. The recognized text is published as a normal user message, so the agent
answers it and the reply is spoken through the speaker.

## Starting it

The script loops forever polling the button, so it must run asynchronously:

```json
{"path":"{CUR_SKILL_DIR}/scripts/push_to_talk.lua","timeout_ms":0,"name":"voice_ptt","exclusive":"voice_ptt"}
```

Call `lua_run_script_async` with that payload. The `startup_voice_push_to_talk`
router rule already does this on `startup/boot_completed`.

## Stopping it

Call `lua_stop_async_job` with `{"name":"voice_ptt"}`. Do this before running any
other script that needs GPIO 0.

## Requirements

- A board that declares `i2s_audio_in` and `i2s_audio_out`, and firmware built
  with `CLAW_CAP_VOICE_PIPELINE_ENABLED`
- ASR and TTS credentials configured on the Voice settings page
