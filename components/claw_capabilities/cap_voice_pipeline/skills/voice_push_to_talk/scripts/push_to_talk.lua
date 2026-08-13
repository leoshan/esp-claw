-- Push-to-talk on one or more buttons.
--
-- Hold a button to capture, release to stop and transcribe. Holding means
-- the utterance ends when the speaker stops instead of after a fixed timeout,
-- so no voice activity detection is needed on the device.
--
-- Runs forever; start it with lua_run_script_async and timeout_ms = 0.

local button = require("button")
local capability = require("capability")
local delay = require("delay")

-- GPIO 0 is the on-board BOOT button, which is tiny and awkward to hold.
-- GPIO 21 is the spare pin an external button goes on: it is not a strapping
-- pin, and it is not claimed by the flash (26-32), the octal PSRAM (33-37),
-- USB (19-20), UART0 (43-44), or this board's audio, I2C and LED peripherals.
local DEFAULT_GPIOS = { 0, 21 }

-- Wire the button between the pin and GND. The driver enables the internal
-- pull-up for an active-low button, so no external resistor is needed.
local ACTIVE_LEVEL = 0
local POLL_INTERVAL_MS = 20

-- Read the pin list from the caller so a different button needs a router rule
-- edit rather than a firmware rebuild. Accepts {21} or "0,21".
local function resolve_gpios()
    local list = type(args) == "table" and args.gpios or nil

    if type(list) == "string" then
        local parsed = {}
        for pin in list:gmatch("%-?%d+") do
            parsed[#parsed + 1] = tonumber(pin)
        end
        if #parsed > 0 then
            return parsed
        end
    elseif type(list) == "table" and #list > 0 then
        return list
    end
    return DEFAULT_GPIOS
end

local function on_press()
    -- Already-listening presses are rejected by the capability itself, so two
    -- buttons held at once cannot restart the capture.
    local ok, _, call_err = capability.call("voice_listen", {})
    if not ok then
        print("push_to_talk: voice_listen failed: " .. tostring(call_err))
    end
end

local function on_release()
    -- Stops the capture task; the audio already streamed is still transcribed.
    local ok, _, call_err = capability.call("voice_stop_listening", {})
    if not ok then
        print("push_to_talk: voice_stop_listening failed: " .. tostring(call_err))
    end
end

local watched = {}

for _, gpio in ipairs(resolve_gpios()) do
    local handle, err = button.new(gpio, ACTIVE_LEVEL)

    if handle then
        button.on(handle, "press_down", on_press)
        button.on(handle, "press_up", on_release)
        watched[#watched + 1] = gpio
    else
        -- One pin already claimed by another peripheral should cost that
        -- button, not the whole watcher.
        print("push_to_talk: skipping GPIO " .. tostring(gpio) .. ": " .. tostring(err))
    end
end

if #watched == 0 then
    error("push_to_talk: no usable button GPIO")
end

print("push_to_talk: hold GPIO " .. table.concat(watched, " or ") .. " to talk")

-- One queue serves every handle, so a single dispatch loop covers them all.
while true do
    button.dispatch()
    delay.delay_ms(POLL_INTERVAL_MS)
end
