------------------------------------------------------------------------------
--- Reproduce tasklist-select teleport with the user's tile layout.
---
--- The user's somewmrc default layout is awful.layout.suit.tile.  Activating
--- a CSD client via the tasklist entry (awful.button(...,1) ->
--- c:activate{action="toggle_minimization"}) runs the layout manage/arrange
--- path.  Check the client does not move when the entry is clicked, both for
--- a plain click and a click that drags slightly (as a real mouse would).
------------------------------------------------------------------------------

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")
local wibox  = require("wibox")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")
local VPOINTER   = utils.binary_or_skip("./build/test-virtual-pointer-client")
if not CSD_CLIENT or not VPOINTER then return end

local function vpointer(action, x, y, extent, btn, dx, dy)
    local args = string.format("%s %s %d %d %s", VPOINTER, action, x, y, extent)
    if btn then args = args .. " " .. btn end
    if dx and dy then args = args .. " " .. dx .. " " .. dy end
    awful.spawn(args)
    async.sleep(0.35)
end

runner.run_async(function()
    local beautiful = require("beautiful")
    beautiful.decorations = "client"

    local s  = screen[1]
    local sg = s.geometry
    local extent = sg.width .. " " .. sg.height

    -- User default layout: tile.
    local t = awful.tag.selected(s)
    t.layout = awful.layout.suit.tile
    async.sleep(0.2)

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

    -- Tiled client: capture geometry after the layout has placed it.
    async.sleep(0.3)
    local initial = c:geometry()
    io.stderr:write(string.format("[TILE] initial (tiled): %d+%d %dx%d layout=%s focus=%s\n",
        initial.x, initial.y, initial.width, initial.height,
        tostring(c.floating), tostring(client.focus == c)))

    -- Give the client pointer/keyboard focus.
    c:activate { context = "test", raise = true }
    async.sleep(0.3)

    local bg = bar:geometry()
    local tx = bg.x + 10
    local ty = bg.y + math.floor(bg.height / 2)

    -- Plain click on the tasklist entry.
    vpointer("click", tx, ty, extent, "left")
    async.sleep(0.5)
    local after_click = c:geometry()
    io.stderr:write(string.format("[TILE] after click:  %d+%d %dx%d focus=%s float=%s\n",
        after_click.x, after_click.y, after_click.width, after_click.height,
        tostring(client.focus == c), tostring(c.floating)))

    local moved = (math.abs(after_click.x - initial.x) > 20) or
                  (math.abs(after_click.y - initial.y) > 20)
    if moved then
        io.stderr:write("[TILE] FAIL: tasklist click moved tiled client\n")
        runner.done("tasklist click moved tiled client")
        return
    end

    -- Now a click WITH slight drag (real mice jitter).  If the press leaked
    -- to the client's headerbar, GTK would start request_move on the motion.
    vpointer("drag", tx, ty, extent, "left", 30, 10)
    async.sleep(0.7)
    local after_drag = c:geometry()
    io.stderr:write(string.format("[TILE] after drag:   %d+%d %dx%d float=%s\n",
        after_drag.x, after_drag.y, after_drag.width, after_drag.height,
        tostring(c.floating)))

    local dragged = (math.abs(after_drag.x - initial.x) > 20) or
                    (math.abs(after_drag.y - initial.y) > 20)
    if dragged then
        io.stderr:write("[TILE] FAIL: tasklist drag moved tiled client\n")
        runner.done("tasklist drag moved tiled client (leaked headerbar press)")
        return
    end

    io.stderr:write("[ALL TESTS] PASS\n")
    c:kill()
    bar.visible = false
    runner.done()
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80