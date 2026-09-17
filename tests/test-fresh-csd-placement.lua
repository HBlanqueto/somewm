--- Fresh CSD client must land INSIDE the workarea at map time.
--
-- A config with only a top wibar and no placement rule must still leave a
-- fresh XDG toplevel inside the strut-shrunk workarea. XDG toplevels report
-- x/y = 0 and, absent a placement rule, the C manage path decides the initial
-- position; it used to leave the client at +0+0, tucked under the 24px top
-- wibar (the "teleports to the very up of the desktop" report).
--
-- The vanilla headless suite never exercised this path: tests/rc.lua has no
-- wibar and no placement rule, so a fresh client mapped against a full-screen
-- workarea and every CSD test stayed green while the live bug persisted.
--
-- Run: make test-one TEST=tests/test-fresh-csd-placement.lua

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")
local ruled  = require("ruled")
local wibar  = require("awful.wibar")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")
if not CSD_CLIENT then return end

local BAR_H = 24

runner.run_async(function()
    -- Same as the user: a top wibar.
    local bar = wibar {
        position = "top",
        screen   = screen.primary,
        height   = BAR_H,
    }
    -- Wait until the wibar's strut has actually shrunk the workarea, exactly
    -- as it is long before the user opens a window in a real session. Spawning
    -- before the strut commits would map the client against a full-screen
    -- workarea, which is not the live scenario.
    for _ = 1, 50 do
        if screen.primary.workarea.y >= BAR_H then break end
        async.sleep(0.05)
    end
    io.stderr:write(string.format("[FRESH-CSD] workarea after wibar: %d+%d %dx%d\n",
        screen.primary.workarea.x, screen.primary.workarea.y,
        screen.primary.workarea.width, screen.primary.workarea.height))

    -- No placement rule is registered, so the fresh client's position is
    -- decided by the C manage path alone.

    -- A fresh CSD client (like a freshly opened Nautilus window).
    awful.spawn(CSD_CLIENT)
    local c = async.wait_for_client("csd-move-test", 5)
    assert(c, "fresh CSD client never mapped")

    -- Let manage + placement settle.
    async.sleep(0.6)

    local geo = c:geometry()
    local wa  = screen.primary.workarea

    io.stderr:write(string.format(
        "[FRESH-CSD] mapped geometry=%dx%d+%d+%d  workarea=%dx%d+%d+%d\n",
        geo.width, geo.height, geo.x, geo.y,
        wa.width, wa.height, wa.x, wa.y))

    -- The visible bug: the client ends up at +0+0, i.e. its headerbar under
    -- the top wibar ("teleports to the very up of the desktop").  With
    -- honor_workarea (default), no_offscreen must keep it at >= workarea.y.
    local inside = geo.x             >= wa.x and
                   geo.y             >= wa.y and
                   geo.x + geo.width <= wa.x + wa.width and
                   geo.y + geo.height<= wa.y + wa.height

    if not inside then
        io.stderr:write(string.format(
            "[FRESH-CSD] BUG: fresh CSD client mapped OUTSIDE the workarea "..
            "(geo=%dx%d+%d+%d workarea=%dx%d+%d+%d) - it is pinned under the "..
            "top wibar, i.e. 'at the very up of the desktop'.\n",
            geo.width, geo.height, geo.x, geo.y,
            wa.width, wa.height, wa.x, wa.y))
    end

    assert(inside, "FRESH-CSD placement regression")

    io.stderr:write("[FRESH-CSD] PASS: fresh CSD client placed inside the workarea\n")

    c:kill()
    bar:remove()
    async.sleep(0.4)

    runner.done()
end)
