------------------------------------------------------------------------------
--- Check whether unminimizing a CSD client from the tasklist relocates it.
---
--- The tasklist entry button is:
---   awful.button({ }, 1, function (c) c:activate { context = "tasklist",
---     action = "toggle_minimization" } end)
--- For a focused client this MINIMIZES; for a minimized client the
--- request::activate unminimizes.  Nothing in that path should move the window.
------------------------------------------------------------------------------

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")
local wibox  = require("wibox")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")

runner.run_async(function()
    local beautiful = require("beautiful")
    beautiful.decorations = "client"

    local s  = screen[1]
    local sg = s.geometry

    local tasklist = awful.widget.tasklist {
        screen  = s,
        filter  = awful.widget.tasklist.filter.currenttags,
        buttons = {
            awful.button({ }, 1, function (c)
                c:activate { context = "tasklist", action = "toggle_minimization" }
            end),
        },
        layout = {
            spacing = 4,
            layout  = wibox.layout.fixed.horizontal,
        },
    }
    local bar = awful.wibar {
        position = "top",
        screen   = s,
        height   = 20,
        widget   = { tasklist, layout = wibox.layout.align.horizontal },
    }
    async.sleep(0.3)

    awful.spawn(CSD_CLIENT .. " nodeco")
    local c = async.wait_for_client("csd-move-test", 5)
    assert(c, "nodeco CSD test client never mapped")
    async.sleep(0.5)

    c.floating = true
    async.sleep(0.05)
    c:geometry({ x = 300, y = 300, width = 400, height = 300 })
    async.sleep(0.2)

    local initial = c:geometry()
    io.stderr:write(string.format("[MIN] initial: %d+%d %dx%d focus=%s minimized=%s\n",
        initial.x, initial.y, initial.width, initial.height,
        tostring(client.focus == c), tostring(c.minimized)))

    -- Make it focused first (simulate: window is active, user clicks entry).
    c:activate { context = "test", raise = true }
    async.sleep(0.3)
    io.stderr:write(string.format("[MIN] after activate: focus=%s minimized=%s\n",
        tostring(client.focus == c), tostring(c.minimized)))

    -- Clicking the entry of a FOCUSED client minimizes it (action toggle).
    c.minimized = true
    async.sleep(0.3)
    local min_geo = c:geometry()
    io.stderr:write(string.format("[MIN] minimized -> visible=%s geo=%d+%d %dx%d\n",
        tostring(c.minimized == false), min_geo.x, min_geo.y,
        min_geo.width, min_geo.height))

    -- Unminimize the way the tasklist would via c:activate.
    c:activate { context = "tasklist", action = "toggle_minimization", raise = true }
    async.sleep(0.3)
    local unmin_geo = c:geometry()
    io.stderr:write(string.format("[MIN] unminimized -> geo=%d+%d %dx%d (was %d+%d)\n",
        unmin_geo.x, unmin_geo.y, unmin_geo.width, unmin_geo.height,
        initial.x, initial.y))

    local moved = (math.abs(unmin_geo.x - initial.x) > 5) or
                  (math.abs(unmin_geo.y - initial.y) > 5)
    if moved then
        io.stderr:write("[MIN] FAIL: unminimize moved the client!\n")
        runner.done("unminimize relocated the client")
        return
    end

    io.stderr:write("[ALL TESTS] PASS\n")
    c:kill()
    bar.visible = false
    runner.done()
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80