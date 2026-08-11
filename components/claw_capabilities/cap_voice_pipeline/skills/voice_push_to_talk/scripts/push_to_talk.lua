-- Push-to-talk on the on-board BOOT button.
--
-- Hold the button to capture, release to stop and transcribe. Holding means
-- the utterance ends when the speaker stops instead of after a fixed timeout,
-- so no voice activity detection is needed on the device.
--
-- Runs forever; start it with lua_run_script_async and timeout_ms = 0.

local button = require("button")
local capability = require("capability")
local delay = require("delay")

-- BOOT button on ESP32-S3-DevKitC-1. It is wired to ground through the switch,
-- so the pressed level is 0.
local BOOT_GPIO = 0
local ACTIVE_LEVEL = 0
local POLL_INTERVAL_MS = 20

local handle, err = button.new(BOOT_GPIO, ACTIVE_LEVEL)
if not handle then
    error("push_to_talk: cannot claim GPIO " .. BOOT_GPIO .. ": " .. tostring(err))
end

button.on(handle, "press_down", function()
    local ok, _, call_err = capability.call("voice_listen", {})
    if not ok then
        print("push_to_talk: voice_listen failed: " .. tostring(call_err))
    end
end)

button.on(handle, "press_up", function()
    -- Stops the capture task; the audio already streamed is still transcribed.
    local ok, _, call_err = capability.call("voice_stop_listening", {})
    if not ok then
        print("push_to_talk: voice_stop_listening failed: " .. tostring(call_err))
    end
end)

print("push_to_talk: hold GPIO " .. BOOT_GPIO .. " (BOOT) to talk")

while true do
    button.dispatch()
    delay.delay_ms(POLL_INTERVAL_MS)
end
