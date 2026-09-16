----------------------------------------------------------------------
--- Reproduce CSD move via headerbar drag, headless (virtual pointer).
---
--- Spawns test-csd-move-client (an xdg_toplevel that sends
--- xdg_toplevel.request_move on drag like GTK), then drives a real
--- pointer press + drag + release via zvirtual-pointer so it traverses
--- the compositor's buttonpress()/motionnotify()/grabber paths.
---
--- The client is spawned twice:
---   1. bound to zxdg_decoration_manager_v1 (CLIENT_SIDE)  - classic
---   2. "nodeco": manager never bound, like GNOME apps e.g. Nautilus
---      that draw their own headerbar and leave c->decoration == NULL
---
--- Asserts the client actually moves (i.e. request_move was honoured).
----------------------------------------------------------------------

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")
local VPOINTER   = utils.binary_or_skip("./build/test-virtual-pointer-client")
if not CSD_CLIENT or not VPOINTER then return end

local MOVE_DX, MOVE_DY = 80, 50

--- Run the virtual pointer client once (async-friendly).
local function vpointer(action, x, y, extent, btn, dx, dy)
    local args = string.format("%s %s %d %d %s", VPOINTER, action, x, y, extent)
    if btn then args = args .. " " .. btn end
    if dx and dy then args = args .. " " .. dx .. " " .. dy end
    awful.spawn(args)
    async.sleep(0.30)
end

--- Drag-spawn a CSD client variant and verify it moves.
local function test_scenario(label, extra_arg, sg, extent)
    io.stderr:write("[CSD-MOVE] scenario: " .. label .. "\n")

    awful.spawn(CSD_CLIENT .. (extra_arg and (" " .. extra_arg) or ""))
    local c = async.wait_for_client("csd-move-test", 5)
    if not c then
        local all = {}
        for _, cc in ipairs(client.get()) do
            table.insert(all, string.format("%s(class=%s name=%s mapped=%s managed=%s)",
                tostring(cc), tostring(cc.class), tostring(cc.name),
                tostring(cc.mapped), tostring(cc.managed)))
        end
        io.stderr:write("clients seen: " .. table.concat(all, "; ") .. "\n")
    end
    assert(c, "CSD test client never mapped (" .. label .. ")")

    -- Give the compositor time to apply CLIENT_SIDE mode.
    async.sleep(0.5)

    local initial = c:geometry()
    assert(initial.width > 0 and initial.height > 0, "empty geometry")
    io.stderr:write(string.format("initial geometry: %d+%d %dx%d\n",
        initial.x, initial.y, initial.width, initial.height))

    -- Hover the pointer over the client (its "titlebar", top area) so the
    -- surface gains pointer focus before the press.
    local hx = initial.x + math.floor(initial.width / 2)
    local hy = initial.y + 10
    vpointer("move", hx, hy, extent)
    async.sleep(0.3)

    -- Drag: press, hold, sweep, release.
    vpointer("drag", hx, hy, extent, "left", MOVE_DX, MOVE_DY)
    async.sleep(0.8)

    -- Verify the window moved.
    local final = c:geometry()
    local moved = (math.abs(final.x - initial.x) > 20) or
                  (math.abs(final.y - initial.y) > 20)
    io.stderr:write(string.format("final geometry:   %d+%d %dx%d\n",
        final.x, final.y, final.width, final.height))

    if not moved then
        runner.done(label .. ": CSD window did not move after headerbar drag")
        return false
    end

    io.stderr:write(string.format(
        label .. ": CSD MOVED OK: %d+%d -> %d+%d (dx=%d dy=%d)\n",
        initial.x, initial.y, final.x, final.y,
        final.x - initial.x, final.y - initial.y))

    c:kill()
    async.sleep(0.4)
    return true
end

runner.run_async(function()
    -- Make sure the CSD client's decoration request is honoured: the global
    -- default must be CLIENT_SIDE or the compositor forces SERVER_SIDE on it.
    -- tests/rc.lua does not ship the user's rules, so set it directly.
    local beautiful = require("beautiful")
    beautiful.decorations = "client"

    local sg     = screen[1].geometry
    local extent = sg.width .. " " .. sg.height

    local ok1 = test_scenario("bound-decoration", nil, sg, extent)

    -- Nautilus-like: decorate via the app itself, never bind the manager.
    local ok2 = test_scenario("nodeco (gnome-style)", "nodeco", sg, extent)

    if ok1 and ok2 then
        io.stderr:write("[ALL TESTS] PASS\n")
        runner.done()
    end
end)
