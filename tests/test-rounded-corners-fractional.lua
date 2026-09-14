---------------------------------------------------------------------------
--- Rounded-corner crop test for fractional-scale (viewport) clients.
---
--- Some clients (e.g. Firefox on a fractional-scaled output) commit an
--- over-scaled buffer and present it via wp_viewport, so the buffer size is
--- not surface_size * buffer_scale. The crop must be computed against the
--- *displayed* box (the scene buffer's src_box/dest), not against
--- buffer/scale -- otherwise the bottom and right corners stay square.
---
--- Spawns the fractional pattern client (1.5x buffer via viewport), enables
--- rounded corners (radius 20), and samples the composed screen:
---
---   * all four frame corners reveal the background (not client content)
---   * interior quadrants keep their colors (BL blue, BR yellow)
---   * the bottom-left diagonal transitions to content at ~radius 20
---
--- Run: make test-one TEST=tests/test-rounded-corners-fractional.lua
---------------------------------------------------------------------------

local awful  = require("awful")
local gears  = require("gears")
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

local APP_ID = "content_pattern_fractional"
local RADIUS = 20

local function find_binary()
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    for _, candidate in ipairs({
        build_dir .. "/test-fractional-pattern-client",
        "./build/test-fractional-pattern-client",
        "./build-test/test-fractional-pattern-client",
    }) do
        local f = io.open(candidate, "r")
        if f then f:close(); return candidate end
    end
    return nil
end

local BINARY = find_binary()
if not BINARY then
    io.stderr:write("SKIP: test-fractional-pattern-client not found\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local function pixel_rgb(raw, x, y)
    ffi.C.cairo_surface_flush(raw)
    local data   = ffi.C.cairo_image_surface_get_data(raw)
    local stride = ffi.C.cairo_image_surface_get_stride(raw)
    local off = y * stride + x * 4
    return data[off + 2], data[off + 1], data[off + 0], data[off + 3]
end

local function near(r, g, b, want, tol)
    return math.abs(r - want.r) <= (tol or 3)
       and math.abs(g - want.g) <= (tol or 3)
       and math.abs(b - want.b) <= (tol or 3)
end

local function screen_pixel(x, y)
    local raw = root.content()
    if not raw then return nil, nil, nil end
    return pixel_rgb(raw, math.max(0, math.floor(x)), math.max(0, math.floor(y)))
end

client.connect_signal("request::manage", function(c)
    if c.class == APP_ID or c.instance == APP_ID then
        c.corner_radius = { radius = RADIUS, enabled = true }
        c.border_width = 0
    end
end)

runner.run_async(function()
    local ok, err = pcall(function()
        local pid = awful.spawn(BINARY)
        assert(type(pid) == "number" and pid > 0, "failed to spawn fractional client")
        local c = async.wait_for_client(APP_ID, 5)
        assert(c, "fractional client never appeared")
        c.floating = true
        c:geometry { x = 60, y = 60, width = 300, height = 240 }
        async.sleep(0.8)   -- let the client re-render at the new size via viewport

        local g = c:geometry()
        local fx, fy = g.x, g.y
        local fw, fh = g.width, g.height

        -- Background just outside the frame.
        local br, bg, bb = screen_pixel(fx - 2, fy - 2)
        local BACKGROUND = { r = br, g = bg, b = bb }
        assert(not near(br, bg, bb, { r = 0, g = 0, b = 255 }),
            "background must not be blue (test would be meaningless)")

        -- (1) All four frame corners reveal the background.
        for _, p in ipairs({
            { fx + 1, fy + 1 },
            { fx + fw - 2, fy + 1 },
            { fx + 1, fy + fh - 1 },
            { fx + fw - 2, fy + fh - 1 },
        }) do
            local r, gg, b = screen_pixel(p[1], p[2])
            assert(near(r, gg, b, BACKGROUND, 6),
                string.format("frame corner @(%d,%d) should reveal the background "
                    .. "rgb(%d,%d,%d), got rgb(%d,%d,%d)",
                    p[1], p[2], br, bg, bb, r, gg, b))
        end

        -- (2) Interior quadrants keep their colors (BL blue, BR yellow).
        local r, gg, b = screen_pixel(fx + 60, fy + fh - 60)
        assert(near(r, gg, b, { r = 0, g = 0, b = 255 }),
            string.format("BL interior should be blue, got rgb(%d,%d,%d)", r, gg, b))
        r, gg, b = screen_pixel(fx + fw - 60, fy + fh - 60)
        assert(near(r, gg, b, { r = 255, g = 255, b = 0 }),
            string.format("BR interior should be yellow, got rgb(%d,%d,%d)", r, gg, b))

        -- (3) Along the bottom-left diagonal, content appears at ~radius 20
        -- (a buffer/scale-based cut would flip to background much earlier).
        r, gg, b = screen_pixel(fx + 1, fy + fh - (RADIUS - 2))
        assert(near(r, gg, b, { r = 0, g = 0, b = 255 }, 60),
            string.format("inside-arc pixel should be blue content, got rgb(%d,%d,%d)", r, gg, b))

        os.execute("kill -9 " .. pid)
        async.wait_for_no_clients(3)
    end)

    if not ok then
        runner.done("test-rounded-corners-fractional: " .. tostring(err))
    else
        runner.done()
    end
end)