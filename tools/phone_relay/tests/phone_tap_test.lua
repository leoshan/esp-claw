-- Isolation test for the "hand": does a click land on a Home Screen icon, with and without the
-- wiggle that phone_agent's locate_pointer does before clicking? No vision model involved.
-- args: {targets = {{x, y}, ...} (0-1000 units), modes = {"open", "wiggle"}, n = trials per pair}
-- Each trial: flick up to the Home Screen, slam the pointer into the corner, move open-loop to the
-- target, (wiggle,) click, then report whether the screen changed. The Mac side grabs a frame on
-- every AIMED line (where the pointer really is) and on every CLICKED line (what opened).
local ble_hid = require("ble_hid")
local capability = require("capability")
local delay = require("delay")
local json = require("json")
local storage = require("storage")

local STEP, STEP_MS = 10, 12      -- same pacing as phone_agent.lua
local HOME_REPORTS = 40
local NUDGE = 40                  -- same wiggle as phone_agent.lua's locate_pointer
local AIM_HOLD_MS = 2500          -- pointer at rest, so the Mac side can capture where it is
local AFTER_CLICK_MS = 3000       -- let the app open (and the mirrored frame arrive)
local SLOT = "taptest"

local a = type(args) == "table" and args or {}
local targets = a.targets or {{154, 340}}
local modes = a.modes or {"open", "wiggle"}
local n = tonumber(a.n) or 3

local span = json.decode(storage.read_file(storage.join_path(storage.get_root_dir(), "phone", "pointer.json")))

local function move_paced(dx, dy)
    while dx ~= 0 or dy ~= 0 do
        local sx = math.max(-STEP, math.min(STEP, dx))
        local sy = math.max(-STEP, math.min(STEP, dy))
        ble_hid.mouse_move(sx, sy)
        dx, dy = dx - sx, dy - sy
        delay.delay_ms(STEP_MS)
    end
end

local function units(x, y)
    return math.floor(x / 1000 * span.w + 0.5), math.floor(y / 1000 * span.h + 0.5)
end

local function home()
    for _ = 1, HOME_REPORTS do
        ble_hid.mouse_move(-127, -127)
        delay.delay_ms(4)
    end
    delay.delay_ms(100)
end

-- flick up from the bottom edge (no rest before lifting: resting opens the app switcher)
local function go_home_screen()
    home()
    move_paced(units(500, 995))
    ble_hid.mouse_button("left", "down")
    move_paced(units(0, -500))
    ble_hid.mouse_button("left", "up")
    ble_hid.release_all()
    delay.delay_ms(2500)
end

local function diff(op)
    local ok, out = capability.call("phone_screen_diff", {op = op, slot = SLOT})
    return ok and json.decode(out) or {ok = false}
end

print(string.format("TAPTEST start span w=%d h=%d n=%d", span.w, span.h, n))
local results = {}
local ok, err = pcall(function()
    for _, t in ipairs(targets) do
        for _, mode in ipairs(modes) do
            local hits = 0
            for i = 1, n do
                go_home_screen()
                diff("mark")
                home()
                move_paced(units(t[1], t[2]))
                if mode == "wiggle" then
                    move_paced(NUDGE, NUDGE)
                    delay.delay_ms(600)
                    move_paced(-NUDGE, -NUDGE)
                    delay.delay_ms(200)
                end
                print(string.format("TAPTEST AIMED target=%d,%d mode=%s trial=%d", t[1], t[2], mode, i))
                delay.delay_ms(AIM_HOLD_MS)
                ble_hid.mouse_button("left", "click")
                ble_hid.release_all()
                print(string.format("TAPTEST CLICKED target=%d,%d mode=%s trial=%d", t[1], t[2], mode, i))
                delay.delay_ms(AFTER_CLICK_MS)
                local d = diff("diff")
                local changed = d.ok and (d.changed or 0) > 0
                hits = hits + (changed and 1 or 0)
                print(string.format("TAPTEST RESULT target=%d,%d mode=%s trial=%d changed=%s n=%s",
                                    t[1], t[2], mode, i, tostring(changed), tostring(d.changed)))
            end
            results[#results + 1] = string.format("%d,%d %s %d/%d", t[1], t[2], mode, hits, n)
        end
    end
end)
pcall(go_home_screen)
pcall(ble_hid.release_all)
print("TAPTEST SUMMARY " .. table.concat(results, " | ") .. (ok and "" or (" ERROR " .. tostring(err))))
