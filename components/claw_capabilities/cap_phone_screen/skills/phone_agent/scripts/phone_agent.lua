-- Phone agent: eyes (phone_look: mirrored screenshot + vision model) and hands (BLE HID mouse +
-- AssistiveTouch). args.mode: "calibrate" | "act" (args.action) | "run" (args.goal, args.max_steps)
-- Pointer model ported from esp32-iphone-agent (ios/IPhoneAgent/Agent/Pointer.swift): the mouse is
-- relative, so every gesture first slams the pointer into the top-left corner, then moves at a
-- constant pace (small reports, fixed interval) so iOS pointer acceleration stays predictable.
local ble_hid = require("ble_hid")
local capability = require("capability")
local delay = require("delay")
local json = require("json")
local storage = require("storage")
local system = require("system")

local STEP = 10            -- max mouse units per report while moving
local STEP_MS = 12         -- report interval
local HOME_REPORTS = 40    -- -127,-127 reports that surely reach the corner
local TAP_SETTLE_MS = 150  -- rest before clicking, so iOS sees a still pointer
local SWIPE_HOLD_MS = 250  -- rest before lifting, so a swipe gets no momentum
-- Lists and pages ignore a pointer drag (iOS treats it as a press, not a pan; picker wheels do pan),
-- so "scroll" uses wheel reports. Measured 2026-10-23 on Settings > Accessibility: reports 100 ms
-- apart move the content 15-19 screenshot pixels each, about linearly; 50 ms or faster starts
-- iOS's inertial scrolling (10 reports at 15 ms flung a whole screen and more), which overshot
-- every target.
local SCROLL_TICK_MS = 100
local SCROLL_PX_PER_TICK = 16
local SCROLL_MAX_TICKS = 40
local CAL_STOPS = {150, 350}
local DEFAULT_SPAN = {w = 480, h = 1060}  -- measured on an iPhone 14 Plus; calibrate refines it
-- mouse units the pointer is wiggled by to find it (pixel diff). A small wiggle of an idle, auto-
-- hidden AssistiveTouch pointer may not even produce a new mirrored frame; 40 units reliably does.
local NUDGE = 40
-- 0-1000 units per axis; a bigger miss is corrected before clicking. About 19 x 21 px on the
-- 540 x 1176 screenshot: well inside any 44 pt button. A flat 15 (8 px across) kept the
-- pointer hopping around a Save button whose every reading was already on it.
local AIM_TOLERANCE_X, AIM_TOLERANCE_Y = 35, 18
-- A list row is ~55 units tall: 18 left the tip 4 units from the row's top line, where "tap #13"
-- (无障碍, 2026-10-25) hit nothing. Row taps aim this close in y.
local ROW_AIM_TOLERANCE_Y = 8
-- iOS clicks at the arrow's tip; the relay measures the arrow's middle. Measured 2026-10-25 on
-- Settings: the tip sat 3-5 px left of and 10-12 px above the measured spot, so the measured spot
-- is aimed this far right/below the target (screenshot pixels).
local POINTER_TIP_DX_PX, POINTER_TIP_DY_PX = 4, 11
-- snap_to_row: a tap closer than this to a list row line (0-1000 units, ~9 px) is refused
local ROW_EDGE = 8
-- a tap that changed more than this fraction of the upper screen opened or left a page (relay
-- /diff "upper"): later DO lines in the same reply were planned for the old page
local NAVIGATED_UPPER = 0.15
local NAV_SLOT = "nav"
-- tap #N / toggle #N: where in a tagged row (relay /frame.jpg?marks=1) the pointer goes, 0-1000 x.
-- 500 is on the label column; switches sit at 410-494 of 540 px on every list seen so far.
local ROW_TAP_X, ROW_SWITCH_X = 500, 837
local MAX_CORRECTIONS = 4
-- a located pointer further than this from the target is a misreading, not a miss to correct.
-- 150 was tuned for Home Screen "snap to item" highlights; with snapping off the readings are
-- right and real open-loop misses reach ~200 (pointer acceleration), so only absurd ones go
local MAX_AIM_ERROR = 400
-- a rough reading (no moved-pointer lobes found: e.g. the pointer sits on a button and the button's
-- hover highlight drowns it) is only trusted this close to the target; the AssistiveTouch button
-- once gave a rough reading 455 units off
local ROUGH_MAX_ERROR = 150
local MAX_POINTER_BOX = 200  -- a diff bbox wider/taller than this is not just the pointer
-- iOS sends a new mirrored frame only when the screen changes (an idle screen can go 2 s without
-- one), so give the wiggled pointer up to ~2.5 s to show up in the diff
local DIFF_POLLS = 10
local DIFF_POLL_MS = 250
-- replaced by the relay's real frame size in main() (it is 540 x 1168 on the iPhone 14 Plus; the
-- assumed 1176 shifted every converted y up to 8 px)
local IMG_W, IMG_H = 540, 1176  -- screenshot the model sees (relay --width 540; iPhone 720x1568 mirror)
local LOOK_ATTEMPTS = 2
local DIFF_ATTEMPTS, DIFF_RETRY_MS = 3, 500  -- relay /mark and /diff requests
local CORRECTION_GAIN = 0.6  -- damp corrections: with 0.85 small corrections overshot and the error flipped sign
-- ...but a big error is corrected almost fully: 2026-10-25 a 0.6 correction of 40-49 units moved
-- the pointer by what was asked (residual 13-17 = 40%), so every tap needed a third reading.
local BIG_ERROR, BIG_CORRECTION_GAIN = 15, 0.9
-- The open-loop move from the corner falls short in y in proportion to y and lands right of the
-- target by a constant (36 first readings, 2026-10-25: ey ~ 0.05 * y, ex ~ -11, units). The move
-- is pre-corrected by these, refined after every first reading (exponential average).
local open_loop = {ky = 0.05, bx = -11}
local OPEN_LOOP_ALPHA = 0.3
-- The mirrored frame of an action arrives 1.5-2 s later (iOS encodes on change, plus relay decode),
-- so "still" means no change over a window longer than that latency; a 300 ms window was fooled
-- into "still" before the action's frames had even arrived.
local STILL_POLLS = 5      -- up to ~8 s
local STILL_MS = 1600
-- With the relay's /still (how long the screen has not changed) a wait ends as soon as the screen
-- has been quiet for QUIET_MS AND the last input is old enough for its frames to have arrived:
-- ~0.7 s for a pointer move, up to ~1.6 s for a tap/type/swipe and the UI's answer. The fixed
-- mark/sleep/diff window above cost ~3.5 s per wait (46% of a step) and is now only the fallback.
local QUIET_MS = 1000
local POINTER_LATENCY_MS = 700
local INPUT_LATENCY_MS = STILL_MS
-- A tap's answer (a new page) reached the relay 1.4-3.6 s after the click (measured 2026-10-25,
-- clean runs); ending the wait at 1.6 s + 1 s quiet reported "did NOT change" before the page
-- arrived, and the model tapped again. After an action the wait now lasts until a change shows up
-- and settles, or this long without any change.
local ACTION_ANSWER_MAX_MS = 4000
local STILL_TIMEOUT_MS = STILL_POLLS * STILL_MS
local STILL_POLL_MAX_MS = 500
-- a rough pointer reading (no moved-pointer lobes) is accepted after this many more diff polls
local ROUGH_EXTRA_POLLS = 2
local WHEEL_MIN_Y, WHEEL_MAX_Y = 60, 940  -- keep wheel drags on screen (0-1000 units)
local LOCK_STALE_S = 180   -- a lock not refreshed for this long belongs to a dead job
-- relay mark slot holding the screen a step decided on; pointer location (wait_still, locate_pointer)
-- keeps using the default slot in between, so this one survives until the step's effect is checked
local STEP_SLOT = "step"
local REPEAT_RETRIES = 1   -- re-asks when the model repeats an action that just changed nothing
local REPEAT_COORD_TOL = 30  -- screenshot pixels: closer coordinates count as the same action
local REPEAT_LIMIT = 2       -- the same actions this many times in a row: the next one is re-asked
-- relay /diff near point (0-1000) whose search disc (radius 500 width units = half the screen
-- width) spans y ~70-530: the content above the on-screen keyboard, which starts at y ~650
local ABOVE_KEYBOARD = {500, 300}

local a = type(args) == "table" and args or {}
local span_hint = {w = 500, h = 1080}  -- replaced by the calibrated span in main()
local phone_dir = storage.join_path(storage.get_root_dir(), "phone")
local calib_path = storage.join_path(phone_dir, "pointer.json")
local lock_path = storage.join_path(phone_dir, "agent.lock")

local function log(...)
    print(string.format(...))
end

-- Milliseconds spent per kind of work in the current step, logged as "timing step N: ..." so slow
-- steps can be broken down from the serial log (look = vision model, still = waiting for a still
-- screen, locate = pointer wiggle and diff polling, confirm = the extra look before tap!).
local timing = {}
local TIMING_KEYS = {"look", "confirm", "still", "locate"}

local function spent(key, since)
    timing[key] = (timing[key] or 0) + (system.millis() - since)
end

local function load_span()
    if storage.exists(calib_path) then
        local ok, data = pcall(json.decode, storage.read_file(calib_path))
        if ok and type(data) == "table" and data.w and data.h then
            return data
        end
    end
    return nil
end

local function save_span(span)
    storage.mkdir(phone_dir)
    storage.write_file(calib_path, json.encode(span))
end

-- Second line of defence after the job's `exclusive: "phone_agent"`: only one script may drive the
-- pointer at a time, whoever started it. The lock carries a heartbeat so a job killed without
-- cleanup (stopped, board reset) does not block forever.
local owner = string.format("%s-%d", tostring(a.mode or "run"), math.random(1, 1e9))

local function lock_holder()
    if not storage.exists(lock_path) then
        return nil
    end
    local ok, data = pcall(json.decode, storage.read_file(lock_path))
    if not ok or type(data) ~= "table" or not data.t then
        return nil
    end
    local age = os.time() - data.t
    if age < 0 or age > LOCK_STALE_S then  -- negative: written during an earlier boot
        return nil
    end
    return data
end

local function refresh_lock()
    storage.write_file(lock_path, json.encode({owner = owner, t = os.time()}))
end

local function acquire_lock()
    storage.mkdir(phone_dir)
    local holder = lock_holder()
    if holder and holder.owner ~= owner then
        error("another phone_agent job is running (" .. tostring(holder.owner) .. "); not starting a second one")
    end
    refresh_lock()
end

local function release_lock()
    local holder = lock_holder()
    if holder and holder.owner == owner then
        storage.remove(lock_path)
    end
end

local function require_hid()
    local st = ble_hid.status()
    if not st.initialized then
        assert(ble_hid.init({name = "esp-claw-hid"}))
        assert(ble_hid.start({name = "esp-claw-hid"}))
        st = ble_hid.status()
    end
    if not st.connected then
        error("BLE HID not connected: pair/connect esp-claw-hid in the phone's Bluetooth settings")
    end
end

local last_input = 0  -- system.millis() of the last HID report: its frames arrive later
local last_action = 0  -- system.millis() of the last click/type/key/scroll (not pointer moves)

local function move_paced(dx, dy)
    last_input = system.millis()
    while dx ~= 0 or dy ~= 0 do
        local sx = math.max(-STEP, math.min(STEP, dx))
        local sy = math.max(-STEP, math.min(STEP, dy))
        ble_hid.mouse_move(sx, sy)
        dx, dy = dx - sx, dy - sy
        delay.delay_ms(STEP_MS)
    end
    last_input = system.millis()
end

local function home()
    for _ = 1, HOME_REPORTS do
        ble_hid.mouse_move(-127, -127)
        delay.delay_ms(4)
    end
    last_input = system.millis()
    delay.delay_ms(100)
end

local function units(span, dx, dy)  -- 0-1000 screen delta -> mouse units
    return math.floor(dx / 1000 * span.w + 0.5), math.floor(dy / 1000 * span.h + 0.5)
end

-- A single failed relay request (seen once: ESP_FAIL right after a save) must not end the run.
local function screen_diff(op, near, slot, pointer)
    local ok, out, err
    for attempt = 1, DIFF_ATTEMPTS do
        ok, out, err = capability.call("phone_screen_diff", {op = op, near = near, slot = slot, pointer = pointer})
        if ok then
            return json.decode(out)
        end
        log("phone_screen_diff %s attempt %d failed: %s", op, attempt, tostring(err or out))
        delay.delay_ms(DIFF_RETRY_MS)
    end
    error("phone_screen_diff failed: " .. tostring(err or out))
end

-- The mirrored frame lags the phone by ~0.3-0.7 s and animations (app launch, sheets) take longer:
-- wait until nothing changed over a window before looking or locating the pointer.
local function wait_still_window()
    for _ = 1, STILL_POLLS do
        screen_diff("mark")
        delay.delay_ms(STILL_MS)
        local d = screen_diff("diff")
        if d.ok and (d.changed or 0) == 0 then
            return true
        end
    end
    return false
end

local relay_has_still = true  -- cleared when the relay answers /still with an error (older relay)

-- latency: how old the last input must be (POINTER_LATENCY_MS after pointer-only moves). Without
-- it (after an action) the screen must also have changed since the action, or the action be
-- ACTION_ANSWER_MAX_MS old.
local function wait_still(latency)
    local t0 = system.millis()
    local after_action = latency == nil
    latency = latency or INPUT_LATENCY_MS
    local result
    while relay_has_still do
        local ok, st = pcall(screen_diff, "still")
        if not ok or not st.ok or type(st.since_ms) ~= "number" then
            log("wait_still: relay /still unavailable (%s); using the fixed window", tostring(ok and json.encode(st) or st))
            relay_has_still = false
            break
        end
        local now = system.millis()
        local wait = math.max(latency - (now - last_input), QUIET_MS - st.since_ms)
        if after_action and wait <= 0 and now - st.since_ms < last_action
                and now - last_action < ACTION_ANSWER_MAX_MS then
            wait = STILL_POLL_MAX_MS / 2  -- quiet, but no answer to the action yet: keep watching
        end
        if wait <= 0 then
            result = true
            break
        end
        if now - t0 > STILL_TIMEOUT_MS then
            result = false
            break
        end
        delay.delay_ms(math.min(wait, STILL_POLL_MAX_MS))
    end
    if result == nil then
        result = wait_still_window()
    end
    spent("still", t0)
    return result
end

-- Wiggle the pointer and see where the screen changed (port of Pointer.findPointer). nil if unsure.
-- near = {x, y}: where we expect it. The relay then picks the changed region that looks like a
-- moved pointer (two opposite lobes, the newer one towards +x +y) nearest to it and measures where
-- the pointer rested, to the pixel; a fading AssistiveTouch button or a hover highlight is skipped.
local function locate_pointer(near)
    local still = wait_still(POINTER_LATENCY_MS)
    local t0 = system.millis()
    screen_diff("mark")
    move_paced(NUDGE, NUDGE)
    local d
    local rough_polls = 0
    for _ = 1, DIFF_POLLS do  -- the mirrored frame arrives ~0.3-0.7 s after the move
        delay.delay_ms(DIFF_POLL_MS)
        d = screen_diff("diff", near, nil, near ~= nil)
        if d.ok and (d.rest or (d.bbox and d.lobes == nil)) then
            break  -- a pointer reading (or a plain change from a relay without pointer=1)
        end
        if d.ok and d.bbox then
            -- only a region without the moved-pointer lobes: give the lobes a few more frames,
            -- then take the rough reading instead of polling out the whole 2.5 s
            rough_polls = rough_polls + 1
            if rough_polls > ROUGH_EXTRA_POLLS then
                break
            end
        end
    end
    -- Leaving the pointer where the nudge put it (and aiming short by the nudge) was tried on
    -- 2026-10-22 and did not converge: the measured nudge is too noisy. Move back instead.
    move_paced(-NUDGE, -NUDGE)
    delay.delay_ms(200)
    spent("locate", t0)
    if d.ok and d.rest then
        return d.rest[1], d.rest[2], false
    end
    if not d.ok or not d.bbox then
        log("locate: no change seen (still=%s, last diff %s)", tostring(still), json.encode(d))
        return nil
    end
    local b = d.bbox
    if b[3] - b[1] > MAX_POINTER_BOX or b[4] - b[2] > MAX_POINTER_BOX then
        log("locate: change too big for the pointer %s", json.encode(d))
        return nil  -- something else moved too (animation, video)
    end
    -- the box spans the pointer at rest and the nudged pointer: take the half nearest to the rest
    -- position (the nudge goes +x +y)
    local half_x, half_y = NUDGE / 2 / span_hint.w * 1000, NUDGE / 2 / span_hint.h * 1000
    return math.floor(d.center[1] - half_x + 0.5), math.floor(d.center[2] - half_y + 0.5), true
end

-- Open-loop move from the corner, then locate the pointer and correct (relative moves are not
-- linear on iOS: pointer acceleration), like the app's "precise" taps.
-- Returns true when the pointer was seen within AIM_TOLERANCE of the target. When the pointer
-- cannot be seen at all, strict=false still returns true (aim at the best estimate): over Home
-- Screen icons iOS turns the pointer into the icon's highlight, so the wiggle shows a highlight
-- jumping between icons instead of a small dot, and refusing every such tap stalls the run.
-- Whether the action did anything is reported to the model after the step either way.
local function goto_point(span, x, y, strict, tol_y)  -- x, y in 0..1000 of the screenshot
    local cx, cy  -- where the open-loop move was aimed (pre-corrected, see open_loop)
    local function from_corner()  -- open-loop estimate, also the restart after a misreading
        home()
        cx, cy = x + open_loop.bx, y + open_loop.ky * y
        move_paced(units(span, cx, cy))
    end
    local function learn(ex, ey)  -- a first reading: what the open-loop move should have added
        open_loop.bx = open_loop.bx + OPEN_LOOP_ALPHA * ((cx - x + ex) - open_loop.bx)
        if y > 200 then
            local ky = (cy - y + ey) / y
            open_loop.ky = open_loop.ky + OPEN_LOOP_ALPHA * (ky - open_loop.ky)
        end
    end
    local function give_up(why, i)
        log("aim %d,%d: %s (try %d)%s", x, y, why, i, strict and "" or "; using the estimate")
        from_corner()
        delay.delay_ms(TAP_SETTLE_MS)
        return not strict
    end
    local misreads = 0
    from_corner()
    for i = 1, MAX_CORRECTIONS + 1 do
        local px, py, rough = locate_pointer({x, y})
        local ex = px and math.floor(x + POINTER_TIP_DX_PX / IMG_W * 1000 - px + 0.5)
        local ey = py and math.floor(y + POINTER_TIP_DY_PX / IMG_H * 1000 - py + 0.5)
        if px then
            log("aim %d,%d: pointer at %d,%d, error %d,%d (try %d%s)", x, y, px, py, ex, ey, i, rough and ", rough" or "")
        end
        local limit = rough and ROUGH_MAX_ERROR or MAX_AIM_ERROR
        if px and not rough and i == 1 and math.abs(ex) <= limit and math.abs(ey) <= limit then
            learn(ex, ey)
        end
        if not px or math.abs(ex) > limit or math.abs(ey) > limit then
            -- one more look from a fresh open-loop move before tapping on the estimate
            misreads = misreads + 1
            if misreads > 1 or i > MAX_CORRECTIONS then
                return give_up(px and "reading not believable" or "pointer not found", i)
            end
            log("aim %d,%d: %s; moving from the corner again", x, y, px and "reading not believable" or "pointer not found")
            from_corner()
            goto continue
        end
        if math.abs(ex) <= AIM_TOLERANCE_X and math.abs(ey) <= (tol_y or AIM_TOLERANCE_Y) then
            delay.delay_ms(TAP_SETTLE_MS)
            return true
        end
        if i > MAX_CORRECTIONS then
            return false
        end
        local gx = math.abs(ex) > BIG_ERROR and BIG_CORRECTION_GAIN or CORRECTION_GAIN
        local gy = math.abs(ey) > BIG_ERROR and BIG_CORRECTION_GAIN or CORRECTION_GAIN
        move_paced(units(span, ex * gx, ey * gy))
        ::continue::
    end
    return false
end

-- fresh screenshot + vision model in one capability call (inspect_image needs an agent context).
-- No coordinate grid: its lines ran through small text such as picker wheel digits.
-- marks: the screenshot gets numbered tags on its list rows (see tagged_rows)
local function look(prompt, key, marks)
    local t0 = system.millis()
    local ok, out, err
    for attempt = 1, LOOK_ATTEMPTS do  -- the vision call can hit a transient network/TLS error
        ok, out, err = capability.call("phone_look", {prompt = prompt, grid = false, marks = marks == true},
                                       {max_output_bytes = 16384})
        if ok then
            spent(key or "look", t0)
            return out
        end
        log("phone_look attempt %d failed: %s", attempt, tostring(err or out))
        delay.delay_ms(2000)
    end
    error("phone_look failed: " .. tostring(err or out))
end

local function calibrate()
    local found = {}
    home()
    local done = 0
    for _, stop in ipairs(CAL_STOPS) do
        move_paced(stop - done, stop - done)
        done = stop
        delay.delay_ms(800)
        local x, y = locate_pointer()
        log("calibrate: moved %d units -> pointer at %s %s", stop, tostring(x), tostring(y))
        if x and y then
            found[#found + 1] = {u = stop, x = x, y = y}
        end
    end
    if #found < 2 or found[2].x == found[1].x or found[2].y == found[1].y then
        error("calibration failed: pointer not found twice; is AssistiveTouch on and mirroring live?")
    end
    -- units per full screen = delta units / delta fraction (corner offsets cancel out)
    local du = found[2].u - found[1].u
    local span = {
        w = math.floor(du / ((found[2].x - found[1].x) / 1000) + 0.5),
        h = math.floor(du / ((found[2].y - found[1].y) / 1000) + 0.5),
    }
    save_span(span)
    log("calibrate: span w=%d h=%d saved to %s", span.w, span.h, calib_path)
    return span
end

local CONFIRM_PROMPT = [[
GOAL: %s
You are about to click a control that cannot be undone: "%s".
The click will land at pixel (%d, %d) of this %d x %d screenshot; the board has already measured
the pointer there, so do not judge by the small grey pointer dot (it is hard to see).
First name what is drawn at that pixel, from this screenshot only (its icon or text, e.g. "orange
checkmark", "plus sign", "Delete button"); the same spot can hold another control on another
screen. Then read the values on screen that the goal is about, exactly as shown (e.g. a time, the
selected days, a typed text, a switch state). Answer YES only if that control is exactly the
intended one AND every one of those values matches the goal AND the goal is not already done on
screen.
Reply in exactly three lines:
CONTROL: <what is at that pixel>
VALUES: <the values as shown> vs goal: <the values the goal needs>
ANSWER: YES, or NO: <what is wrong>
%s]]

-- Irreversible taps ("tap! X Y label"): aim, wait for a still screen, then let the model confirm on
-- a fresh screenshot that the control at the measured spot is the intended one before clicking.
-- x, y: 0-1000 units of the aimed spot.
local function confirm_click(ctx, label, x, y)
    wait_still(POINTER_LATENCY_MS)  -- only the pointer moved since the screen was last still
    local px, py = math.floor(x / 1000 * IMG_W + 0.5), math.floor(y / 1000 * IMG_H + 0.5)
    local reply = look(string.format(CONFIRM_PROMPT, ctx.goal or "(single action)", label ~= "" and label or "?",
                                     px, py, IMG_W, IMG_H, ctx.rules or ""), "confirm")
    log("confirm %q -> %s", label, reply)
    return reply:match("ANSWER:%s*YES") ~= nil, reply
end

-- Drag one column of a picker wheel by `rows` rows (rows > 0: towards bigger values, the content
-- moves up). row_h: row height in 0-1000 units of the screen height.
local function drag_wheel(span, x, y, rows, row_h)
    local remaining = rows * row_h  -- > 0: finger moves up
    -- every drag must start on the wheel's selected row; a long move is split into several drags
    -- because the finger has to stay on screen
    local room = remaining > 0 and (y - WHEEL_MIN_Y) or (WHEEL_MAX_Y - y)
    if room < row_h then
        return false
    end
    room = math.floor(room / row_h) * row_h  -- whole rows per drag, so each drag ends on a row
    while math.abs(remaining) >= 1 do
        local d = math.max(-room, math.min(room, remaining))
        if not goto_point(span, x, y) then
            return false
        end
        ble_hid.mouse_button("left", "down")
        move_paced(units(span, 0, -d))
        delay.delay_ms(SWIPE_HOLD_MS)
        ble_hid.mouse_button("left", "up")
        remaining = remaining - d
        if math.abs(remaining) >= 1 then
            wait_still()  -- let the wheel settle before grabbing it again
        end
    end
    return true
end

-- A tap inside a list row goes to the row's middle: the vision model's y is often 30-80 px off
-- (2026-10-25) and rows are ~70 px tall, so its taps landed on row edges and opened the
-- neighbouring row. The relay finds the separator lines around the point; anything that is not
-- row-shaped (buttons, nav bars) is left alone. A point within ROW_EDGE of a row line is not
-- snapped but refused (nil, why): it is on the border of two rows, and snapping picks one of them
-- by luck. 2026-10-25: y 865 and 870 on the 通用/无障碍 border both opened 通用, and a row half
-- under Settings' floating search field has its only visible edge there too. x, y and the result:
-- 0-1000 units.
local function snap_to_row(x, y)
    local called, ok, out = pcall(capability.call, "phone_screen_diff", {op = "row", near = {x, y}})
    local decoded, row = false, nil
    if called and ok and out then
        decoded, row = pcall(json.decode, out)
    end
    if not decoded or type(row) ~= "table" then
        return y
    end
    local edge = row.top and y - row.top < ROW_EDGE and row.top
        or row.bottom and row.bottom - y < ROW_EDGE and row.bottom
    if edge then
        log("row edge: y %d is %d from a row line at %d", y, math.abs(y - edge), edge)
        return nil, "the point is on the line between two list rows (or at a row's covered edge)"
    end
    if not row.row or not row.center then
        return y, false
    end
    if row.center ~= y then
        log("row snap y %d -> %d (row %d-%d)", y, row.center, row.top, row.bottom)
    end
    return row.center, true
end

-- A left click. ble_hid's own click (down, 40 ms, up) sometimes left the press open on the phone:
-- 2026-10-26, the first tap on Settings' 无障碍 row left the row highlighted and nothing opened,
-- with both reports sent without error, until the next pointer move ended the touch elsewhere. A
-- longer press and a second release report make sure iOS sees the finger lift where it pressed.
local CLICK_HOLD_MS, CLICK_RELEASE_REPEAT_MS = 80, 30
local function click()
    local ok, err = ble_hid.mouse_button("left", "down")
    if not ok then
        pcall(ble_hid.release_all)
        return false, err
    end
    delay.delay_ms(CLICK_HOLD_MS)
    local up_ok, up_err = ble_hid.mouse_button("left", "up")
    delay.delay_ms(CLICK_RELEASE_REPEAT_MS)
    local again = ble_hid.mouse_button("left", "up")
    if not up_ok and not again then
        pcall(ble_hid.release_all)
        return false, up_err
    end
    return true
end

-- The rows numbered on the last marked look, {[n] = center y in 0-1000 units}; empty when the relay
-- can't say (then "#N" actions fail and the model falls back to coordinates).
local function tagged_rows()
    local ok, d = pcall(screen_diff, "rows")
    local rows = {}
    if ok and type(d) == "table" and d.ok and type(d.rows) == "table" then
        for _, r in ipairs(d.rows) do
            rows[r.n] = r.center
        end
    end
    return rows
end

-- "tap #7", "tap! #7 Save", "toggle #7" -> x, y (0-1000) of that row's label / switch, or nil, why
local function row_point(verb, rest, ctx)
    local n = tonumber(rest:match("^#(%d+)"))
    local y = n and ctx.rows and ctx.rows[n]
    if not y then
        return nil, "there is no tag #" .. tostring(n) .. " on this screen"
    end
    return verb == "toggle" and ROW_SWITCH_X or ROW_TAP_X, y
end

local function act(span, line, ctx)
    ctx = ctx or {}
    local verb, bang, rest = line:match("^%s*(%a+)(!?)%s*(.-)%s*$")
    verb = verb and verb:lower() or ""
    local n = {}
    for v in rest:gmatch("-?%d+") do
        n[#n + 1] = tonumber(v)
    end
    local by_tag = rest:match("^#%d+") ~= nil
    if by_tag and (verb == "tap" or verb == "toggle") then
        -- a tagged row: its middle is known exactly, so no snapping and no edge check
        local x, y = row_point(verb, rest, ctx)
        if not x then
            return false, y .. "; not tapped"
        end
        if not goto_point(span, x, y, bang == "!", ROW_AIM_TOLERANCE_Y) then
            return false, "could not aim at row " .. rest:match("^#%d+") .. "; not tapped"
        end
        if bang == "!" then
            local label = rest:gsub("^#%d+%s*", "")
            local yes, reply = confirm_click(ctx, label, x, y)
            if not yes then
                return false, "confirmation said " .. reply:gsub("%s+", " "):sub(1, 160) .. "; not tapped"
            end
        end
        local clicked, click_err = click()
        if not clicked then
            return false, "the click did not reach the phone (" .. tostring(click_err) .. "); not tapped"
        end
    elseif verb == "move" and #n >= 2 then  -- aim only, no click (debugging / hover)
        if not goto_point(span, n[1], n[2]) then
            return false, "could not aim at " .. n[1] .. "," .. n[2]
        end
    elseif verb == "tap" and #n >= 2 then
        local snapped, in_row = snap_to_row(n[1], n[2])
        local why = in_row
        if not snapped then
            return false, why .. "; not tapped. Scroll so the row is fully visible away from bars, "
                .. "then tap the middle of its label"
        end
        n[2] = snapped
        -- an irreversible tap! is never clicked on an estimate: the pointer must be seen on target
        if not goto_point(span, n[1], n[2], bang == "!", in_row == true and ROW_AIM_TOLERANCE_Y or nil) then
            return false, "could not aim at " .. n[1] .. "," .. n[2] .. "; not tapped"
        end
        if bang == "!" then
            local label = rest:gsub("^%s*-?%d+%s+-?%d+%s*", "")
            local yes, reply = confirm_click(ctx, label, n[1], n[2])
            if not yes then
                return false, "confirmation said " .. reply:gsub("%s+", " "):sub(1, 160) .. "; not tapped"
            end
        end
        local clicked, click_err = click()
        if not clicked then
            return false, "the click did not reach the phone (" .. tostring(click_err) .. "); not tapped"
        end
    elseif verb == "swipe" and #n >= 4 then
        if not goto_point(span, n[1], n[2]) then
            return false, "could not aim at swipe start " .. n[1] .. "," .. n[2] .. "; not swiped"
        end
        ble_hid.mouse_button("left", "down")
        move_paced(units(span, n[3] - n[1], n[4] - n[2]))
        delay.delay_ms(SWIPE_HOLD_MS)
        ble_hid.mouse_button("left", "up")
    elseif verb == "scroll" and #n >= 3 then
        -- the pointer only has to be over the list: open-loop, no locating
        home()
        move_paced(units(span, n[1], n[2]))
        delay.delay_ms(TAP_SETTLE_MS)
        local ticks = math.min(SCROLL_MAX_TICKS, math.floor(math.abs(n[3]) / SCROLL_PX_PER_TICK + 0.5))
        for _ = 1, ticks do
            ble_hid.mouse_scroll(n[3] > 0 and 1 or -1)
            delay.delay_ms(SCROLL_TICK_MS)
        end
    elseif verb == "wheel" and #n >= 4 then
        if not drag_wheel(span, n[1], n[2], n[3], n[4]) then
            return false, "could not aim at wheel " .. n[1] .. "," .. n[2] .. "; not moved"
        end
    elseif verb == "type" then
        ble_hid.text(rest)
    elseif verb == "key" then
        local keys = {}
        for k in rest:gmatch("[^+%s]+") do
            keys[#keys + 1] = k:upper()
        end
        if #keys == 1 then
            ble_hid.key(keys[1])
        else
            ble_hid.combo(table.unpack(keys))
        end
    elseif verb == "home" then
        ble_hid.combo("GUI", "H")  -- hardware-keyboard shortcut for the Home Screen
    elseif verb == "wait" then
        delay.delay_ms(2000)
    else
        return false, "unknown action: " .. line
    end
    ble_hid.release_all()
    last_input = system.millis()
    if verb ~= "move" then
        last_action = last_input
    end
    return true
end

local ACTOR_PROMPT = [[
You operate an iPhone through a mouse pointer. You see the current screen.
GOAL: %s

Recent steps (oldest first):
%s

Reply in this format, nothing else:
APP: <the app on screen, e.g. Settings, Clock, Home Screen>
CONTEXT: <what this screen has, any of: %s; or none>
THINK: <first check the last action against the screen, then what you do now and why>
DO: <action>
Give several DO lines (max 4) only when they do not depend on seeing the result. A tap that
opens another page or sheet is always the last DO line: never tap where you expect the next
page to be (the actions after such a tap are skipped).
Red tags with white numbers are drawn at the left edge of list rows (some also sit on section
headers and gaps); they are not part of the phone screen. For a list row or its switch use the tag:
"tap #N" / "toggle #N", where N is the number on the tag on the same line as the row's label (read
it, do not count rows). Your pixel coordinates for rows are often a row off; the tags are exact.
Coordinates (for everything without a tag) are PIXELS of this screenshot, which is %d x %d (x 0-%d
left->right, y 0-%d top->bottom); aim at the visual center of the element.
Only tap things you can see on THIS screenshot. The same spot can be another button on another
screen: name what you tap.
Each recent step says whether the screen changed after it. "screen did NOT change" means the action
missed or had no effect: do not assume it worked; aim again more precisely or try another way, never
the same action again. An action marked "not tapped" / "not moved" was NOT performed.
Before redoing a step, read the screen: if it already shows the wanted value, that step is done.
When the result the goal asks for is visible on screen, reply done at once. Never put done in the
same reply as another action: act, then confirm on the next screenshot.
Actions:
  tap #N                 the list row with tag N (opens it / selects it)
  toggle #N              the switch at the right end of the row with tag N
  tap X Y                buttons, icons, fields and anything without a tag
  tap! X Y LABEL         anything that cannot be undone (save, done, delete, send, buy, confirm): you
  tap! #N LABEL          are asked to confirm on a fresh screenshot before the click happens
  scroll X Y DY          lists and pages under X,Y: DY > 0 shows what is further DOWN (the content
                         moves up about DY pixels), DY < 0 further UP; at most ~600 at a time
  swipe X1 Y1 X2 Y2      a drag (sliders, moving an item); it does NOT scroll lists or pages
  wheel X Y ROWS ROW_H   move a picker column by ROWS rows
  type TEXT (ASCII) | key COMBO (enter, backspace, cmd+a ...) | home | wait
  done ANSWER (goal reached and verified on screen) | fail REASON
Rules for this app and screen (they follow the APP and CONTEXT you report):
%s
]]

-- Prompt layers (see prompts/README.md): ACTOR_PROMPT above is the core sent every step; the rest is
-- read from prompt files so a rule only shows up where it applies. DATA/phone/prompts overrides the
-- skill's own prompts/ file by file, so prompts can be tuned on the board without reflashing.
local WIDGET_TAGS = {"keyboard", "picker", "sheet", "list", "toggle"}
local prompt_roots = {storage.join_path(phone_dir, "prompts")}
do
    local source = debug.getinfo(1, "S").source:gsub("^@", "")
    local skill_dir = source:match("^(.*)/scripts/[^/]+$")
    if skill_dir then
        prompt_roots[#prompt_roots + 1] = skill_dir .. "/prompts"
    end
end
local prompt_cache = {}

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- A prompt file from the first root that has it; "" when none does.
local function read_prompt(rel)
    if prompt_cache[rel] == nil then
        prompt_cache[rel] = ""
        for _, root in ipairs(prompt_roots) do
            local path = root .. "/" .. rel
            if storage.exists(path) then
                local ok, text = pcall(storage.read_file, path)
                if ok and text then
                    prompt_cache[rel] = trim(text)
                    break
                end
            end
        end
    end
    return prompt_cache[rel]
end

local function comma_words(s)
    local words = {}
    for raw in (s or ""):gmatch("[^,]+") do
        local w = trim(raw):lower()
        if w ~= "" then
            words[#words + 1] = w
        end
    end
    return words
end

-- playbooks/*.md: "match:" keywords and "contexts:" preloaded widget tags, then the rules.
local function load_playbooks()
    local books, seen = {}, {}
    for _, root in ipairs(prompt_roots) do
        local ok, entries = pcall(storage.listdir, root .. "/playbooks")
        for _, e in ipairs(ok and type(entries) == "table" and entries or {}) do
            local name = e.name
            if name and name:match("%.md$") and not seen[name] then
                seen[name] = true
                local book = {name = name:gsub("%.md$", ""), match = {}, contexts = {}}
                local body = {}
                for line in (read_prompt("playbooks/" .. name) .. "\n"):gmatch("([^\n]*)\n") do
                    local key, value = line:match("^(%a+):%s*(.*)$")
                    if #body == 0 and (key == "match" or key == "contexts") then
                        book[key] = comma_words(value)
                    else
                        body[#body + 1] = line
                    end
                end
                book.rules = trim(table.concat(body, "\n"))
                books[#books + 1] = book
            end
        end
    end
    return books
end

local function mentions(text, words)
    text = (text or ""):lower()
    for _, w in ipairs(words) do
        if text:find(w, 1, true) then
            return true
        end
    end
    return false
end

-- The widget tags in a reply's CONTEXT: line, as a set.
local function reported_tags(reply)
    local tags, line = {}, (reply:match("CONTEXT:%s*([^\n]*)") or ""):lower()
    for _, tag in ipairs(WIDGET_TAGS) do
        if line:find(tag, 1, true) then
            tags[tag] = true
        end
    end
    return tags
end

-- The rules text for this step: platform, then the playbooks that match the goal or the app the model
-- reported, then the widget rules for the active tags (reported now or a step ago, or preloaded by
-- a playbook). Returns the text and a short description for the log.
local function layered_rules(books, goal, app, tags)
    local parts, used, active = {read_prompt("platform.md")}, {}, {}
    for tag in pairs(tags) do
        active[tag] = true
    end
    for _, book in ipairs(books) do
        if mentions(goal, book.match) or mentions(app, book.match) then
            parts[#parts + 1] = book.rules
            used[#used + 1] = book.name
            for _, tag in ipairs(book.contexts) do
                active[tag] = true
            end
        end
    end
    local tag_list = {}
    for _, tag in ipairs(WIDGET_TAGS) do
        if active[tag] then
            parts[#parts + 1] = read_prompt("widgets/" .. tag .. ".md")
            tag_list[#tag_list + 1] = tag
        end
    end
    local kept = {}
    for _, p in ipairs(parts) do
        if p ~= "" then
            kept[#kept + 1] = p
        end
    end
    return table.concat(kept, "\n\n"),
           string.format("playbooks=%s widgets=%s", table.concat(used, ","), table.concat(tag_list, ","))
end

-- The model answers in screenshot pixels (vision models are more reliable with pixels than with an
-- abstract 0-1000 scale); act() works in 0-1000 units of the screen. Converts the coordinates of
-- tap/tap!/move/swipe, the X, Y of scroll and the X, Y, ROW_H of wheel; keeps everything else
-- (rows, labels, scroll distances, which stay in screenshot pixels).
local function pixels_to_units(line)
    local verb, bang, rest = line:match("^%s*(%a+)(!?)%s*(.-)%s*$")
    if not verb or rest:match("^#%d+") then  -- "tap #7": a row tag, not a coordinate
        return line
    end
    local lv = verb:lower()
    local axes  -- per numeric argument: "x", "y" or false (keep)
    if lv == "tap" or lv == "move" then
        axes = {"x", "y"}
    elseif lv == "swipe" then
        axes = {"x", "y", "x", "y"}
    elseif lv == "scroll" then
        axes = {"x", "y", false}
    elseif lv == "wheel" then
        axes = {"x", "y", false, "y"}
    else
        return line
    end
    local i = 0
    local converted = rest:gsub("-?%d+", function(v)
        i = i + 1
        local axis = axes[i]
        if axis == nil or axis == false then
            return v
        end
        return tostring(math.floor(tonumber(v) / (axis == "x" and IMG_W or IMG_H) * 1000 + 0.5))
    end)
    return lv .. bang .. " " .. converted
end

-- The pointer rests in the top-left corner, inside the status bar the relay's diff ignores, so
-- pointer moves never count as a screen change (and it does not cover anything the model reads).
local function park_and_mark()
    home()
    wait_still()  -- let animations finish so the model never decides on a stale screen
    screen_diff("mark", nil, STEP_SLOT)
end

-- Did the step change the screen since park_and_mark? true / false, nil when the relay can't tell
-- (also when it can't be reached: the effect is a hint for the model, not worth ending the run),
-- plus a note for the model. typed: the step typed on the on-screen keyboard, whose keys flash on
-- every press; then only the content above the keyboard counts (round 9 kept reporting "changed"
-- while the time stayed 12:07, because of the key flashes).
local function step_changed(typed)
    home()
    local ok, d, above = pcall(function()
        wait_still()
        local whole = screen_diff("diff", nil, STEP_SLOT)
        return whole, typed and screen_diff("diff", ABOVE_KEYBOARD, STEP_SLOT) or nil
    end)
    if not ok then
        log("effect: unknown (%s)", tostring(d))
        return nil, ""
    end
    if not d.ok then
        log("effect: unknown (%s)", json.encode(d))
        return nil, ""
    end
    log("effect: changed=%s peak=%s bbox=%s%s", tostring(d.changed), tostring(d.peak), json.encode(d.bbox),
        above and (" above_keyboard=" .. json.encode(above.bbox)) or "")
    local changed = (d.changed or 0) > 0
    if changed and above and above.ok and not above.bbox then
        return false, " (only keyboard keys flashed)"
    end
    return changed, ""
end

-- After a tap followed by more DO lines: did it open or leave a page (or a sheet)? Those lines were
-- planned for the old screen. Typing, the keyboard sliding up and picker wheels stay below
-- NAVIGATED_UPPER; false when the relay can't tell.
local function tap_navigated()
    local ok, d = pcall(function()
        wait_still()
        return screen_diff("diff", nil, NAV_SLOT)
    end)
    if not ok or type(d) ~= "table" or not d.ok or not d.upper then
        return false
    end
    log("tap effect: upper=%.2f", d.upper)
    return d.upper > NAVIGATED_UPPER
end

local function first_action(reply)
    local line = reply:match("DO:%s*([^\n]+)")
    return line and line:lower():gsub("%s+", " "):gsub("%s$", "") or nil
end

-- All DO lines of a reply as {verb, numbers, text}, for comparing steps.
local function action_list(reply)
    local list = {}
    for line in reply:gmatch("DO:%s*([^\n]+)") do
        local verb, rest = line:match("^%s*(%a+!?)%s*(.-)%s*$")
        local nums = {}
        for v in (rest or ""):gmatch("-?%d+") do
            nums[#nums + 1] = tonumber(v)
        end
        list[#list + 1] = {verb = (verb or ""):lower(), nums = nums, text = (rest or ""):lower():gsub("%s+", " ")}
    end
    return list
end

-- Same actions again, give or take a few pixels: "tap 280 300; type 07" and "tap 267 287; type 07"
-- are the same move (round 9 repeated it 7 times; exact string matching never noticed).
local function same_actions(x, y)
    if not x or not y or #x == 0 or #x ~= #y then
        return false
    end
    for i = 1, #x do
        local p, q = x[i], y[i]
        if p.verb ~= q.verb then
            return false
        end
        if p.verb == "type" or p.verb == "key" then
            if p.text ~= q.text then
                return false
            end
        elseif #p.nums ~= #q.nums then
            return false
        else
            for k = 1, math.min(#p.nums, 2) do  -- the coordinates; later numbers (rows) must match
                if math.abs(p.nums[k] - q.nums[k]) > REPEAT_COORD_TOL then
                    return false
                end
            end
            for k = 3, #p.nums do
                if p.verb ~= "swipe" and p.nums[k] ~= q.nums[k] then
                    return false
                end
            end
        end
    end
    return true
end

local function run(span, goal, max_steps)
    local history = {}
    local ctx = {goal = goal}
    local last_actions, last_changed  -- DO lines of the previous step and their effect
    local repeats = 0                 -- how many steps in a row repeated the previous one's actions
    local books = load_playbooks()
    local app, tags, prev_tags = "", {}, {}  -- from the last reply's APP:/CONTEXT: lines
    park_and_mark()
    for step = 1, max_steps do
        refresh_lock()
        local step_t0 = system.millis()
        timing = {}
        local recent = #history > 0 and table.concat(history, "\n", math.max(1, #history - 5)) or "(none)"
        local active = {}
        for tag in pairs(prev_tags) do
            active[tag] = true  -- kept one more step: a reported widget can flicker out of a reply
        end
        for tag in pairs(tags) do
            active[tag] = true
        end
        local rules, layers = layered_rules(books, goal, app, active)
        ctx.rules = rules
        log("step %d prompt: %s (%d chars of rules)", step, layers, #rules)
        local prompt = string.format(ACTOR_PROMPT, goal, recent, table.concat(WIDGET_TAGS, ", "),
                                     IMG_W, IMG_H, IMG_W, IMG_H, rules)
        local reply = look(prompt, nil, true)
        ctx.rows = tagged_rows()  -- the rows tagged on this very screenshot
        for _ = 1, REPEAT_RETRIES do  -- the same move that just did nothing will do nothing again
            local same = same_actions(action_list(reply), last_actions)
            if not same or (last_changed ~= false and repeats + 1 < REPEAT_LIMIT) then
                break
            end
            local why = last_changed == false and "it changed nothing last time"
                or string.format("you already tried it %d times in a row without reaching the goal", repeats + 1)
            log("step %d: model repeated %s (%s); asking again", step, first_action(reply), why)
            reply = look(prompt .. "\nYou just proposed \"" .. (first_action(reply) or "") .. "\" again, but " .. why
                         .. ". Choose a different action (another way, e.g. retype the whole value).\n", nil, true)
            ctx.rows = tagged_rows()
        end
        log("step %d model:\n%s", step, reply)
        app = reply:match("APP:%s*([^\n]*)") or app
        prev_tags, tags = tags, reported_tags(reply)
        local did = {}
        local lines = {}
        for line in reply:gmatch("DO:%s*([^\n]+)") do
            lines[#lines + 1] = line
        end
        for li, line in ipairs(lines) do
            local verb = line:match("^%s*(%a+)")
            verb = verb and verb:lower() or ""
            if (verb == "done" or verb == "fail") and #did > 0 then
                -- 2026-10-26 "toggle #11" + "done" in one reply: done before the switch was seen to
                -- move. The next step looks at the result and says done (or not) again.
                log("step %d: %s after an action in the same reply ignored; checking the result first", step, verb)
                break
            end
            if verb == "done" then
                log("timing step %d: total=%d look=%d (ms)", step, system.millis() - step_t0, timing.look or 0)
                log("done: %s", (line:gsub("^%s*%a+%s*", "")))
                return true
            elseif verb == "fail" then
                log("fail: %s", (line:gsub("^%s*%a+%s*", "")))
                return false
            end
            -- a tap with more DO lines after it: remember the screen, to see whether it opened a page
            local check_nav = verb == "tap" and li < #lines and pcall(screen_diff, "mark", nil, NAV_SLOT)
            local ok, err = act(span, pixels_to_units(line), ctx)
            did[#did + 1] = line .. (ok and "" or (" -> " .. err))
            if not ok then
                break  -- later DO lines assumed this one worked
            end
            if check_nav and tap_navigated() then
                -- 2026-10-25: "tap 无障碍; tap 300 645" — the first tap opened 通用 instead, and the
                -- second, aimed at the expected page, opened iPhone 储存空间 there
                did[#did] = did[#did] .. " -> opened another screen; the rest of this reply was skipped"
                break
            end
            if verb == "scroll" then
                -- the content moved: coordinates in later DO lines point at other rows now, and a
                -- second blind scroll was what overshot the target in the 2026-10-23 runs
                break
            end
        end
        local effect = ""
        if #did == 0 then
            did[1] = "(no action parsed)"
            last_changed = nil
        else
            local typed = reply:match("DO:%s*[Tt][Yy][Pp][Ee]%s") ~= nil
            local note
            last_changed, note = step_changed(typed)
            effect = last_changed == true and " -> screen changed"
                or last_changed == false and (" -> screen did NOT change" .. note) or ""
            pcall(screen_diff, "mark", nil, STEP_SLOT)  -- the settled screen the next step decides on
        end
        local actions = action_list(reply)
        repeats = same_actions(actions, last_actions) and repeats + 1 or 0
        last_actions = actions
        history[#history + 1] = string.format("%d. %s%s", step, table.concat(did, "; "), effect)
        log("history %s", history[#history])
        local total, parts, rest = system.millis() - step_t0, {}, system.millis() - step_t0
        for _, key in ipairs(TIMING_KEYS) do
            parts[#parts + 1] = string.format("%s=%d", key, timing[key] or 0)
            rest = rest - (timing[key] or 0)
        end
        log("timing step %d: total=%d %s other=%d (ms)", step, total, table.concat(parts, " "), rest)
    end
    log("fail: step limit (%d) reached", max_steps)
    return false
end

local function main()
    local mode = a.mode or "run"
    require_hid()
    local span = load_span()
    if mode == "calibrate" then
        calibrate()
        return
    end
    span_hint = span or DEFAULT_SPAN
    local ok_size, st = pcall(screen_diff, "still")
    if ok_size and st.w and st.h then
        IMG_W, IMG_H = st.w, st.h
    end
    log("screenshot size %dx%d", IMG_W, IMG_H)
    if not span then
        log("pointer not calibrated yet; using defaults w=%d h=%d (run mode=calibrate)", DEFAULT_SPAN.w, DEFAULT_SPAN.h)
        span = DEFAULT_SPAN
    end
    if mode == "act" then
        local ok, err = act(span, tostring(a.action or ""), {goal = a.goal})
        log(ok and ("ok: " .. tostring(a.action)) or ("error: " .. err))
    elseif mode == "run" then
        if not a.goal or a.goal == "" then
            error("mode=run needs args.goal")
        end
        log("run %s: %s", owner, a.goal)
        run(span, a.goal, tonumber(a.max_steps) or 15)
    else
        error("unknown mode: " .. tostring(mode))
    end
end

acquire_lock()
local ok, err = pcall(main)
pcall(ble_hid.release_all)
release_lock()
if not ok then
    pcall(log, "error: %s", tostring(err))
    error(err, 0)
end
