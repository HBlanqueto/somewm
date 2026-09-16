------------------------------------------------------------------------------
--- Reproduce "mod4+right-click resize breaks subsequent CSD headerbar drag".
---
--- User report: after using the mod4+right-click client resize binding from
--- rc.lua, dragging the app's own (CSD) headerbar no longer moves the window.
---
--- Test strategy (no keyboard injection available, so no mod4):
---   1. Spawn a nodeco CSD client (Nautilus-like: never binds the decoration
---      manager, c->decoration == NULL).
---   2. Baseline: headerbar drag moves the client (CSD move works).
---   3. Attach a no-mod right-click CLIENT mousebinding that performs the SAME
---      resize path as rc.lua's mod4+right:
---        c:activate { context="mouse_click", action="mouse_resize" }
---      i.e. awful.mouse.client.resize(c).
---   4. Drive a real right-click drag over the client with the virtual pointer,
---      which starts the resize mousegrabber (button goes through
---      buttonpress() -> luaA_client_button_check -> binding -> grabber).
---   5. Assert mousegrabber.isrunning() == false after the right-button release.
---   6. Redo the headerbar drag; assert the client still moves.
---
--- If step 5 fails, a grabber is left running: every later press is consumed
--- by event_handle_mousegrabber() before the client (or some_client_start_move)
--- ever sees it, which is exactly the reported breakage.
------------------------------------------------------------------------------

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")
local VPOINTER   = utils.binary_or_skip("./build/test-virtual-pointer-client")
if not CSD_CLIENT or not VPOINTER then return end

local MOVE_DX, MOVE_DY = 80, 50
local RESIZE_DX = 60

--- Run the virtual pointer client once (async-friendly).
local function vpointer(action, x, y, extent, btn, dx, dy)
    local args = string.format("%s %s %d %d %s", VPOINTER, action, x, y, extent)
    if btn then args = args .. " " .. btn end
    if dx and dy then args = args .. " " .. dx .. " " .. dy end
    awful.spawn(args)
    async.sleep(0.30)
end

runner.run_async(function()
    local beautiful = require("beautiful")
    beautiful.decorations = "client"

    local sg     = screen[1].geometry
    local extent = sg.width .. " " .. sg.height

    io.stderr:write("[CSD-RESIZE] spawning nodeco (gnome-style) client\n")
    awful.spawn(CSD_CLIENT .. " nodeco")
    local c = async.wait_for_client("csd-move-test", 5)
    assert(c, "nodeco CSD test client never mapped")
    async.sleep(0.5)

    local initial = c:geometry()
    assert(initial.width > 0 and initial.height > 0, "empty geometry")

    -- Pointer over the client's headerbar area.
    local hx = initial.x + math.floor(initial.width / 2)
    local hy = initial.y + 10
    vpointer("move", hx, hy, extent)

    -- Step 2: baseline headerbar drag must move the client.
    vpointer("drag", hx, hy, extent, "left", MOVE_DX, MOVE_DY)
    async.sleep(0.8)
    local after_drag = c:geometry()
    local moved = (math.abs(after_drag.x - initial.x) > 20) or
                  (math.abs(after_drag.y - initial.y) > 20)
    io.stderr:write(string.format(
        "[CSD-RESIZE] baseline headerbar drag: %d+%d -> %d+%d moved=%s\n",
        initial.x, initial.y, after_drag.x, after_drag.y, tostring(moved)))
    assert(moved, "baseline CSD headerbar drag did not move the client")

    -- Step 3: attach the same resize path rc.lua uses for mod4+right-click.
    -- (no modifier: the test harness has no keyboard injection; the resize
    --  grabber lifecycle is what we are exercising, not the modifier)
    local resize_button = awful.button({}, 3, function(cc)
        cc:activate { context = "mouse_click", action = "mouse_resize" }
    end)
    c:append_mousebinding(resize_button)
    io.stderr:write("[CSD-RESIZE] attached right-click client resize binding\n")

    -- Place pointer over the client body (left of centre, not on the headerbar).
    local cx = after_drag.x + math.floor(after_drag.width / 2)
    local cy = after_drag.y + math.floor(after_drag.height / 2)
    vpointer("move", cx, cy, extent)

    -- Step 4: real right-click drag -> buttonpress -> luaA_client_button_check
    -- fires the binding -> awful.mouse.client.resize -> mousegrabber.run.
    local before_resize = c:geometry()
    vpointer("drag", cx, cy, extent, "right", RESIZE_DX, 30)
    async.sleep(0.8)

    -- Step 5: the grabber must have stopped on the right-button release.
    local still_running = mousegrabber.isrunning()
    io.stderr:write(string.format(
        "[CSD-RESIZE] after right-drag release: mousegrabber.isrunning()=%s\n",
        tostring(still_running)))
    assert(not still_running,
        "mousegrabber is STILL RUNNING after right-click resize release - "
        .. "it will swallow every later press in event_handle_mousegrabber()")

    local after_resize = c:geometry()
    io.stderr:write(string.format(
        "[CSD-RESIZE] resize result: %dx%d+%d+%d (was %dx%d+%d+%d)\n",
        after_resize.width, after_resize.height, after_resize.x, after_resize.y,
        before_resize.width, before_resize.height, before_resize.x, before_resize.y))

    -- Step 6: the subsequent CSD headerbar drag must still move the client.
    -- Recompute the headerbar position from the CURRENT (post-resize) geometry.
    local hx2 = after_resize.x + math.floor(after_resize.width / 2)
    local hy2 = after_resize.y + 10
    vpointer("move", hx2, hy2, extent)
    vpointer("drag", hx2, hy2, extent, "left", MOVE_DX, -MOVE_DY)
    async.sleep(0.8)
    local final = c:geometry()
    local still_moves = (math.abs(final.x - after_resize.x) > 20) or
                        (math.abs(final.y - after_resize.y) > 20)
    io.stderr:write(string.format(
        "[CSD-RESIZE] post-resize headerbar drag: %d+%d -> %d+%d moved=%s\n",
        after_resize.x, after_resize.y, final.x, final.y, tostring(still_moves)))
    assert(still_moves,
        "CSD headerbar drag stopped working after the right-click resize")

    io.stderr:write("[CSD-RESIZE] PASS: grabber stops and headerbar drag still works\n")
    c:kill()
    runner.done()
end)