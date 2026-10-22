-- Click timing test: why does a BLE HID click sometimes reach iOS as a long press?
-- Run only on a harmless page (Settings > Accessibility > Touch > Haptic Touch, test image).
-- args.mode:
--   "move":  slam into the corner, then move open-loop to {x, y} (0-1000 units); no click
--   "nudge": relative move by {dx, dy} mouse units; no click
--   "run":   click groups with the pointer kept still; every click logs CLICKTEST lines
-- The Mac side grabs frames after each CLICKTEST CLICK line to see tap vs long press.
local ble_hid = require("ble_hid")
local delay = require("delay")
local json = require("json")
local storage = require("storage")

local STEP, STEP_MS = 10, 12      -- same pacing as phone_agent.lua
local HOME_REPORTS = 40
local GAP_MS = 3000               -- between clicks: long enough for the Mac to see the result

local a = type(args) == "table" and args or {}
local mode = a.mode or "run"

local function move_paced(dx, dy)
    while dx ~= 0 or dy ~= 0 do
        local sx = math.max(-STEP, math.min(STEP, dx))
        local sy = math.max(-STEP, math.min(STEP, dy))
        ble_hid.mouse_move(sx, sy)
        dx, dy = dx - sx, dy - sy
        delay.delay_ms(STEP_MS)
    end
end

local function res(ok, why)
    return ok and "ok" or ("FAIL:" .. tostring(why))
end

if mode == "move" then
    local span = json.decode(storage.read_file(storage.join_path(storage.get_root_dir(), "phone", "pointer.json")))
    for _ = 1, HOME_REPORTS do
        ble_hid.mouse_move(-127, -127)
        delay.delay_ms(4)
    end
    delay.delay_ms(100)
    move_paced(math.floor(a.x / 1000 * span.w + 0.5), math.floor(a.y / 1000 * span.h + 0.5))
    print(string.format("CLICKTEST MOVED to %d,%d (span %dx%d)", a.x, a.y, span.w, span.h))
    return
end

if mode == "nudge" then
    move_paced(a.dx or 0, a.dy or 0)
    print(string.format("CLICKTEST NUDGED %d,%d", a.dx or 0, a.dy or 0))
    return
end

-- group name, count, hold ms (nil = the firmware's own click)
local groups = a.groups or {
    -- D holds long enough that the mirrored frame (0.5-2 s late) shows the long-press preview
    {"A", 10, nil}, {"B", 10, 40}, {"C", 5, 100}, {"D", 3, 2500},
}

print("CLICKTEST START")
for _, g in ipairs(groups) do
    local name, count, hold = g[1], g[2], g[3]
    for i = 1, count do
        local down, up
        print(string.format("CLICKTEST PRESS group=%s n=%d", name, i))
        if hold == nil then
            down = "-"
            up = res(ble_hid.mouse_button("left", "click"))
        else
            down = res(ble_hid.mouse_button("left", "down"))
            delay.delay_ms(hold)
            up = res(ble_hid.mouse_button("left", "up"))
        end
        local rel = res(ble_hid.release_all())
        print(string.format("CLICKTEST CLICK group=%s n=%d hold=%s down=%s up=%s release=%s",
                            name, i, tostring(hold or 40), down, up, rel))
        delay.delay_ms(GAP_MS)
    end
end
print("CLICKTEST END")
