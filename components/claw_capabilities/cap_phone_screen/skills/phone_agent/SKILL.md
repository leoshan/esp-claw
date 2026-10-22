---
{
  "name": "phone_agent",
  "description": "Operate the user's iPhone: see its screen (AirPlay mirroring to this board) and tap/swipe/type on it through BLE HID with AssistiveTouch.",
  "metadata": {
    "cap_groups": [
      "cap_phone_screen",
      "cap_llm_inspect",
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Phone Agent

Use this skill when the user asks the device to do something ON THEIR PHONE (open an app, change a
setting, set an alarm, read what is on the phone screen, ...).

## Requirements (check with `phone_screen_status` first)
- The phone mirrors its screen to this board: Control Center -> Screen Mirroring -> the receiver name
  (`airplay` is `streaming`, `last_au_age_ms` is small). The LAN relay must be running.
- The phone is paired with the board's BLE HID (`esp-claw-hid`) and AssistiveTouch is on, so the
  board's mouse moves a pointer on the phone. Start HID first with the `ble_hid` skill's
  `start_ble_hid.lua` if it is not connected.
- The pointer is calibrated once (`mode: calibrate`); the result is kept in `phone/pointer.json`
  under the DATA root.

## Run a whole task (preferred)
```json
{"path":"{CUR_SKILL_DIR}/scripts/phone_agent.lua","args":{"mode":"run","goal":"Open Settings and turn on Wi-Fi","max_steps":20},"timeout_ms":900000,"name":"phone_agent","exclusive":"phone_agent"}
```
Rules:
- Start it yourself with `lua_run_script_async` and ALWAYS pass `"exclusive": "phone_agent"`: only one
  phone job may drive the pointer at a time (the script also refuses to start while another one
  holds its lock). Never start a second phone job (not even `calibrate`) while one is running, and
  never pass `replace: true` for it. If the start is rejected, a phone job is already running: poll
  that one instead.
- Do NOT spawn a sub-agent for it: every extra agent opens more HTTPS connections and the board's
  network gets flaky.
- Poll the job with `lua_get_async_job` and `"wait_ms": 60000`: each call waits up to a minute for
  the job to end, so a long task needs only a few calls. Never poll without `wait_ms`: a phone task
  runs 5-15 minutes and fast polling hits the tool-call limit, so the user never gets the result.
  Each step logs THINK/DO lines and the job ends with `done: <answer>` or `fail: <reason>`; report
  that line to the user.
- Put the user's exact constraints in the goal (e.g. "do not modify existing alarms").
- Irreversible buttons (save, delete, send, buy) are confirmed by the model on a fresh screenshot
  before the click; number wheels (alarm/timer times) are set by tapping the selected number and
  typing the digits, and dragged a few rows at a time only when typing does not take.
- The script's prompt is layered (`{CUR_SKILL_DIR}/prompts/`, see its README): core rules, iPhone
  rules, per-widget rules (keyboard, picker, sheet, list, toggle) and per-app playbooks
  (`prompts/playbooks/*.md`). App-specific lessons belong in a playbook, not in the core prompt.
  Files under the DATA root's `phone/prompts/` override them without reflashing.
- After acting, each step logs `effect: changed=N` (0 = the screen did not change; the model is told)
  and a `history N. <actions> -> screen changed / did NOT change` line.

## Other modes
- Calibrate the pointer (run once after pairing, or when taps miss):
  `{"mode":"calibrate"}`
- One action, coordinates 0-1000 relative to the screenshot:
  `{"mode":"act","action":"tap 500 300"}`, `"swipe 500 800 500 300"`, `"type hello"`, `"key enter"`, `"home"`
- Just look: call `phone_screenshot`, then `inspect_image` with the returned path.

## Notes
- Typing is ASCII only (US keyboard layout).
- iOS does not mirror password fields, Apple Pay or DRM video; a locked phone stops mirroring.
- The agent acts with the user's hands: only do what the user asked.
