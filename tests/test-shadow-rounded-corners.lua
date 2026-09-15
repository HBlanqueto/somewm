---------------------------------------------------------------------------
--- Shadow follows rounded corners (per-corner).
---
--- The compositor shadow adopts the window's effective per-corner radii, so
--- a window that is rounded on top but square at the bottom casts a shadow
--- with exactly that contour. Spawns the 4-quadrant content-pattern client
--- (TL=red, TR=green, BL=blue, BR=yellow), rounds only the top corners
--- (TL=TR=24, BL=BR=0), and gives it a magenta shadow grown by `spread` with
--- no offset, then samples the composed screen via root.content():
---
---   * the window's cut top corners reveal the magenta shadow behind them,
---     while the square bottom corners show the window's own content
---   * the shadow's own bottom corners are square (its falloff glow reaches
---     right up to the window corner) while the top corners are cut back
---
--- Then it flips the rounding to the bottom corners (TL=TR=0, BL=BR=24):
---
---   * the shadow's bottom corners become cut and its top corners become
---     square -- the shadow re-adapted to the new window specification
---
--- Run: make test-one TEST=tests/test-shadow-rounded-corners.lua
---------------------------------------------------------------------------

local awful  = require("awful")
local async  = require("_async")
local runner = require("_runner")

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
    io.stderr:write("SKIP: ffi not available (non-LuaJIT build); pixel sampling needs FFI\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

ffi.cdef[[
void cairo_surface_flush(void *surface);
unsigned char *cairo_image_surface_get_data(void *surface);
int cairo_image_surface_get_stride(void *surface);
int cairo_image_surface_get_width(void *surface);
int cairo_image_surface_get_height(void *surface);
]]

local APP_ID = "content_pattern_test"
local RAD_TOP = 24     -- rounded-corner radius for the top corners
local RAD_BOT = 0      -- square bottom corners
local SPREAD  = 8      -- shadow grows this many px beyond the frame
local FALLOFF = 12     -- shadow falloff radius (keeps every corner a buffer)
local MAGENTA = { 255, 0, 255 }

local function find_binary()
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    for _, candidate in ipairs({
        build_dir .. "/test-content-pattern-client",
        "./build/test-content-pattern-client",
        "./build-test/test-content-pattern-client",
    }) do
        local f = io.open(candidate, "r")
        if f then f:close(); return candidate end
    end
    return nil
end

local BINARY = find_binary()
if not BINARY then
    io.stderr:write("SKIP: test-content-pattern-client not found\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local function pixel_rgb(raw, x, y)
    ffi.C.cairo_surface_flush(raw)
    local data   = ffi.C.cairo_image_surface_get_data(raw)
    local stride = ffi.C.cairo_image_surface_get_stride(raw)
    local off = math.max(0, math.floor(y)) * stride + math.max(0, math.floor(x)) * 4
    return data[off + 2], data[off + 1], data[off + 0], data[off + 3]
end

local function screen_pixel(x, y)
    local raw = root.content()
    if not raw then return nil, nil, nil, nil end
    return pixel_rgb(raw, x, y)
end

local function near(r, g, b, want, tol)
    tol = tol or 60
    return math.abs(r - want[1]) <= tol
       and math.abs(g - want[2]) <= tol
       and math.abs(b - want[3]) <= tol
end

-- Bright magenta anywhere in the box (the corner-patch glow). Robust against
-- the exact half-pixel position of the shadow's own corner contour.
local function box_has_bright_magenta(x0, y0, x1, y1)
    local raw = root.content()
    for y = y0, y1 do
        for x = x0, x1 do
            local r, gg, b = pixel_rgb(raw, x, y)
            if r > 200 and gg < 80 and b > 200 then return true end
        end
    end
    return false
end

-- A shadow corner is "square" when its falloff glow reaches right up to the
-- window corner; it is "cut" (rounded) when the glow is pulled back by the
-- arc. The box sits just outside the frame corner, in the glow zone.
local function shadow_corner_square(gx, gy, w, h, corner)
    if corner == "tl" then
        return box_has_bright_magenta(gx - 12, gy - 14, gx - 4, gy - 8)
    elseif corner == "tr" then
        return box_has_bright_magenta(gx + w + 4, gy - 14, gx + w + 12, gy - 8)
    elseif corner == "bl" then
        return box_has_bright_magenta(gx - 12, gy + h + 8, gx - 4, gy + h + 14)
    else
        return box_has_bright_magenta(gx + w + 4, gy + h + 8, gx + w + 12, gy + h + 14)
    end
end

local function wait_for_painted(c, timeout_secs)
    async.wait_for_condition(function()
        local raw = c.content
        if not raw then return false end
        local w = ffi.C.cairo_image_surface_get_width(raw)
        local h = ffi.C.cairo_image_surface_get_height(raw)
        if w <= 4 or h <= 4 then return false end
        for _, p in ipairs({{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}}) do
            local _, _, _, a = pixel_rgb(raw, math.floor(w * p[1]), math.floor(h * p[2]))
            if a < 200 then return false end
        end
        return true
    end, timeout_secs or 5, 0.02)
end

local pids = {}

runner.run_async(function()
    local ok, err = pcall(function()
        local con = client.connect_signal
        con("request::manage", function(c)
            if c.class == APP_ID or c.instance == APP_ID then
                c.corner_radius = { radius = { RAD_TOP, RAD_TOP, RAD_BOT, RAD_BOT }, enabled = true }
                c.border_width = 0
                c.shadow = {
                    enabled = true,
                    radius = FALLOFF,
                    offset_x = 0,
                    offset_y = 0,
                    spread  = SPREAD,
                    opacity = 1.0,
                    color   = "#ff00ff",
                    follow_corners = true,
                }
            end
        end)

        local pid = awful.spawn(BINARY)
        assert(type(pid) == "number" and pid > 0, "failed to spawn pattern client")
        table.insert(pids, pid)

        local c = async.wait_for_client(APP_ID, 5)
        assert(c, "pattern client never appeared")

        -- Property round-trips: per-corner radii survive, shadow follows.
        local cfg = c.corner_radius
        assert(type(cfg) == "table", "c.corner_radius should be a table")
        local cr = cfg.corner_radii or (cfg.radius and type(cfg.radius) == "table" and cfg.radius)
        assert(cr, "per-corner radii should be reported")
        assert(cr[1] == RAD_TOP and cr[2] == RAD_TOP
            and cr[3] == RAD_BOT and cr[4] == RAD_BOT,
            "per-corner radii round-trip mismatch")
        local s = c.shadow
        assert(type(s) == "table", "c.shadow should be a table")
        assert(s.follow_corners == true, "c.shadow.follow_corners should be true")

        c.floating = true
        c:geometry { x = 173, y = 109, width = 320, height = 260 }
        local g = c:geometry()
        async.sleep(0.2)

        wait_for_painted(c, 5)
        async.sleep(0.2)   -- let apply_geometry_to_wlroots place the shadow

        local gx, gy, w, h = g.x, g.y, g.width, g.height

        -- Background must not be magenta or every assertion is meaningless.
        local br, bg, bb = screen_pixel(gx - 40, gy - 40)
        assert(not near(br, bg, bb, MAGENTA, 60),
            string.format("background rgb(%d,%d,%d) must not be magenta", br, bg, bb))

        io.stderr:write(("[shadow-rc] rounded-top geometry %d,%d %dx%d bg rgb(%d,%d,%d)\n")
            :format(gx, gy, w, h, br, bg, bb))

        -- The window's cut top corners reveal the shadow behind them.
        local r, gg, b = screen_pixel(gx + 1, gy + 1)
        assert(near(r, gg, b, MAGENTA),
            string.format("window TL cut should reveal the shadow rgb(%d,%d,%d), got rgb(%d,%d,%d)",
                MAGENTA[1], MAGENTA[2], MAGENTA[3], r, gg, b))
        r, gg, b = screen_pixel(gx + w - 1, gy + 1)
        assert(near(r, gg, b, MAGENTA),
            string.format("window TR cut should reveal the shadow rgb(%d,%d,%d), got rgb(%d,%d,%d)",
                MAGENTA[1], MAGENTA[2], MAGENTA[3], r, gg, b))

        -- The shadow's own bottom corners are square (glow reaches the corner).
        assert(shadow_corner_square(gx, gy, w, h, "bl"),
            "shadow BL corner should be square")
        assert(shadow_corner_square(gx, gy, w, h, "br"),
            "shadow BR corner should be square")

        -- The shadow's own top corners are rounded (glow cut back by the arc).
        assert(not shadow_corner_square(gx, gy, w, h, "tl"),
            "shadow TL corner should be cut (rounded)")
        assert(not shadow_corner_square(gx, gy, w, h, "tr"),
            "shadow TR corner should be cut (rounded)")

        io.stderr:write("[shadow-rc] top-rounded contour verified\n")

        -- Flip the rounding to the bottom corners. The content crop needs a
        -- fresh client commit to re-apply, so bounce fullscreen (which forces
        -- a configure/commit) and land back on the floating geometry.
        c.corner_radius = { radius = { RAD_BOT, RAD_BOT, RAD_TOP, RAD_TOP }, enabled = true }
        c.fullscreen = true
        async.sleep(0.3)
        wait_for_painted(c, 5)
        c.fullscreen = false
        async.sleep(0.3)
        wait_for_painted(c, 5)
        async.sleep(0.2)

        -- The shadow's own corners flipped to match (no client commit needed).
        assert(shadow_corner_square(gx, gy, w, h, "tl"),
            "shadow TL corner should be square after flip")
        assert(shadow_corner_square(gx, gy, w, h, "tr"),
            "shadow TR corner should be square after flip")
        assert(not shadow_corner_square(gx, gy, w, h, "bl"),
            "shadow BL corner should be cut after flip")
        assert(not shadow_corner_square(gx, gy, w, h, "br"),
            "shadow BR corner should be cut after flip")

        local g2 = c:geometry()
        io.stderr:write(("[shadow-rc] after flip geometry %d,%d %dx%d\n")
            :format(g2.x, g2.y, g2.width, g2.height))
        for _, p in ipairs({
            { "tl", gx + 1, gy + 1 }, { "tr", gx + w - 1, gy + 1 },
            { "bl", gx + 1, gy + h - 1 }, { "br", gx + w - 1, gy + h - 1 },
        }) do
            local pr, pg, pb = screen_pixel(p[2], p[3])
            io.stderr:write(("[shadow-rc] window %s corner (%d,%d) rgb=%d,%d,%d\n")
                :format(p[1], p[2], p[3], pr, pg, pb))
        end

        -- The window's cut bottom corners now reveal the shadow.
        r, gg, b = screen_pixel(gx + 1, gy + h - 1)
        assert(near(r, gg, b, MAGENTA),
            string.format("window BL cut should reveal the shadow rgb(%d,%d,%d), got rgb(%d,%d,%d)",
                MAGENTA[1], MAGENTA[2], MAGENTA[3], r, gg, b))
        r, gg, b = screen_pixel(gx + w - 1, gy + h - 1)
        assert(near(r, gg, b, MAGENTA),
            string.format("window BR cut should reveal the shadow rgb(%d,%d,%d), got rgb(%d,%d,%d)",
                MAGENTA[1], MAGENTA[2], MAGENTA[3], r, gg, b))

        io.stderr:write("[shadow-rc] shadow re-adapted to the new corner spec: ok\n")
    end)

    for _, pid in ipairs(pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    for _, c in ipairs(client.get()) do c:kill() end
    async.wait_for_no_clients(3)

    if not ok then
        runner.done("test-shadow-rounded-corners: " .. tostring(err))
    else
        runner.done()
    end
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80