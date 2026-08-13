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
local IDLE_LEVEL = 1
local POLL_INTERVAL_MS = 20

-- Read the pin list from the caller so a different button needs a router rule
-- edit rather than a firmware rebuild. Accepts {21} or "0,21".
local function resolve_gpios()
    local list = type(args) == "table" and args.gpios or nil
    local parsed = {}

    if type(list) == "string" then
        for pin in list:gmatch("%-?%d+") do
            parsed[#parsed + 1] = tonumber(pin)
        end
    elseif type(list) == "table" then
        for _, pin in ipairs(list) do
            parsed[#parsed + 1] = tonumber(pin)
        end
    end
    if #parsed == 0 then
        parsed = DEFAULT_GPIOS
    end

    -- JSON numbers arrive as Lua floats, which would print as "21.0" and make
    -- every log line about this pin look like a different pin.
    local pins = {}
    for _, pin in ipairs(parsed) do
        pins[#pins + 1] = math.tointeger(pin) or pin
    end
    return pins
end

local function on_press(gpio)
    -- Already-listening presses are rejected by the capability itself, so two
    -- buttons held at once cannot restart the capture.
    local ok, _, call_err = capability.call("voice_listen", {})
    if ok then
        print("push_to_talk: GPIO " .. gpio .. " pressed -> listening")
    else
        print("push_to_talk: GPIO " .. gpio .. " voice_listen failed: " .. tostring(call_err))
    end
end

local function on_release(gpio)
    -- Stops the capture task; the audio already streamed is still transcribed.
    local ok, _, call_err = capability.call("voice_stop_listening", {})
    if not ok then
        print("push_to_talk: GPIO " .. gpio .. " voice_stop_listening failed: " ..
              tostring(call_err))
    end
end

local watched = {}

for _, gpio in ipairs(resolve_gpios()) do
    local handle, err = button.new(gpio, ACTIVE_LEVEL)

    if handle then
        button.on(handle, "press_down", function() on_press(gpio) end)
        button.on(handle, "press_up", function() on_release(gpio) end)
        watched[#watched + 1] = {
            gpio = gpio,
            handle = handle,
            level = button.get_key_level(handle),
        }
    else
        -- One pin already claimed by another peripheral should cost that
        -- button, not the whole watcher.
        print("push_to_talk: skipping GPIO " .. tostring(gpio) .. ": " .. tostring(err))
    end
end

if #watched == 0 then
    error("push_to_talk: no usable button GPIO")
end

for _, b in ipairs(watched) do
    -- An idle pin sits at 1, held there by the internal pull-up. A pin already
    -- reading 0 is either wired to GND permanently or has a stuck switch, and
    -- would look exactly like "the button does nothing".
    print(string.format("push_to_talk: GPIO %d idle level %s%s", b.gpio, tostring(b.level),
                        b.level == IDLE_LEVEL and "" or "  <-- expected " .. IDLE_LEVEL))
end
print("push_to_talk: hold GPIO " .. table.concat(
    (function()
        local pins = {}
        for _, b in ipairs(watched) do pins[#pins + 1] = b.gpio end
        return pins
    end)(), " or ") .. " to talk")

-- One queue serves every handle, so a single dispatch loop covers them all.
while true do
    button.dispatch()

    -- Report raw pin transitions alongside the button events they should
    -- produce. A transition with no "pressed" line means the button library
    -- rejected it; no transition at all means the press never reached the pin,
    -- which is wiring rather than software.
    for _, b in ipairs(watched) do
        local level = button.get_key_level(b.handle)

        if level ~= b.level then
            b.level = level
            print("push_to_talk: GPIO " .. b.gpio .. " level -> " .. tostring(level))
        end
    end

    delay.delay_ms(POLL_INTERVAL_MS)
end
