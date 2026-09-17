------------------------------------------------------------------------------
--- Reproduce "selecting a CSD client from the tasklist teleports it to the top".
---
--- User report: clicking the entry for a CSD app (Nautilus, which draws its
--- own headerbar and never binds the decoration manager -> c->decoration is
--- NULL) in the tasklist/wibar teleports the window to the top of the desktop.
---
--- Suspected mechanism: the tasklist entry button runs c:activate during
--- buttonpress() Stage 1, then Stage 2/drawin check finds no C-level button
--- and returns 0, so buttonpress() falls through to
--- wlr_seat_pointer_notify_button() which forwards the press to the seat's
--- focused surface. If pointer focus is left on the CSD client, GTK sees a
--- headerbar press and (with real pointer motion) starts request_move, which
--- somewm now honours (e340486) -> the window jumps to mresize's placement.
---
--- This test creates the user's exact wibar layout (tasklist with the same
--- buttons) and drives a real virtual-pointer click + drag on the entry,
--- asserting the client geometry does NOT change.
------------------------------------------------------------------------------

local runner = require("_runner")
local async  = require("_async")
local utils  = require("_utils")
local awful  = require("awful")
local wibox  = require("wibox")

local CSD_CLIENT = utils.binary_or_skip("./build/test-csd-move-client")
local VPOINTER   = utils.binary_or_skip("./build/test-virtual-pointer-client")
if not CSD_CLIENT or not VPOINTER then return end

local MOVE_DX, MOVE_DY = 60, 40

--- Run the virtual pointer client once (async-friendly).
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

    -- Recreate the caller's setup: a wibar at the top whose first widget is
    -- the tasklist with the same buttons as somewmrc.lua.
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

    -- Spawn the Nautilus-like CSD client (never binds the deco manager).
    io.stderr:write("[TELEPORT] spawning nodeco (gnome-style) client\n")
    awful.spawn(CSD_CLIENT .. " nodeco")
    local c = async.wait_for_client("csd-move-test", 5)
    assert(c, "nodeco CSD test client never mapped")
    async.sleep(0.5)

    -- Float it near the top so its headerbar is just under the wibar.
    c.floating = true
    async.sleep(0.05)
    c:geometry({ x = 200, y = 40, width = 500, height = 400 })
    async.sleep(0.2)

    local initial = c:geometry()
    io.stderr:write(string.format("[TELEPORT] initial geometry: %d+%d %dx%d\n",
        initial.x, initial.y, initial.width, initial.height))

    -- Give the client pointer focus by hovering its headerbar area.
    local hx = initial.x + math.floor(initial.width / 2)
    local hy = initial.y + 10
    vpointer("move", hx, hy, extent)
    async.sleep(0.3)

    -- Click the tasklist entry. The tasklist is at the top of the wibar; its
    -- first entry starts at the left edge of the bar. Click anywhere on the
    -- wibar that hits the tasklist widget: with a single client on the tag the
    -- entry spans the beginning of the bar.
    local bg = bar:geometry()
    local tx = bg.x + 10
    local ty = bg.y + math.floor(bg.height / 2)
    io.stderr:write(string.format("[TELEPORT] clicking tasklist at %d,%d (bar %d+%d %dx%d)\n",
        tx, ty, bg.x, bg.y, bg.width, bg.height))

    -- A plain click on the entry: must NOT move the client.
    vpointer("click", tx, ty, extent, "left")
    async.sleep(0.6)

    local after_click = c:geometry()
    local moved = (math.abs(after_click.x - initial.x) > 20) or
                  (math.abs(after_click.y - initial.y) > 20)
    io.stderr:write(string.format("[TELEPORT] after tasklist click: %d+%d %dx%d moved=%s\n",
        after_click.x, after_click.y, after_click.width, after_click.height, tostring(moved)))

    -- Even click+drag on the entry must not drag the client (a leaked headerbar
    -- press followed by motion is exactly what GTK interprets as request_move).
    vpointer("drag", tx, ty, extent, "left", MOVE_DX, MOVE_DY)
    async.sleep(0.8)

    local after_drag = c:geometry()
    local dragged = (math.abs(after_drag.x - initial.x) > 20) or
                    (math.abs(after_drag.y - initial.y) > 20)
    io.stderr:write(string.format("[TELEPORT] after tasklist drag:  %d+%d %dx%d moved=%s\n",
        after_drag.x, after_drag.y, after_drag.width, after_drag.height, tostring(dragged)))

    if moved then
        io.stderr:write("[TELEPORT] FAIL: tasklist CLICK moved the client\n")
        runner.done("tasklist click teleported the client")
        return
    end
    if dragged then
        io.stderr:write("[TELEPORT] FAIL: tasklist CLICK+DARG moved the client\n")
        runner.done("tasklist drag moved the client (leaked headerbar press)")
        return
    end

    io.stderr:write("[ALL TESTS] PASS\n")

    c:kill()
    bar.visible = false
    runner.done()
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80