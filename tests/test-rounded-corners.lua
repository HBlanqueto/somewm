---------------------------------------------------------------------------
--- Rounded-corner crop test (compositor-level feature).
---
--- Clients get true rounded corners: the toplevel buffer is cropped so the
--- cut corner is transparent and reveals whatever is behind the window.
--- Spawns the 4-quadrant content-pattern client (TL=red), enables rounded
--- corners for it (radius 8), places it floating at a known position, and
--- samples the composed screen capture:
---
---   * the cut corner pixel shows the background behind the window, not red
---   * a pixel just inside the arc still shows the quadrant color (red)
---   * a pixel in the opposite (uncut) interior stays red
---   * in fullscreen the corners are not cut (corner pixel is red again)
---   * back out of fullscreen the crop returns
---
--- Run: make test-one TEST=tests/test-rounded-corners.lua
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

local APP_ID = "content_pattern_test"
local MASK_RADIUS = 8

-- Resolve which build dir the harness used so we find the right binary under
-- both `make test-integration` style SOMEWM pointers.
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
    local off = y * stride + x * 4
    return data[off + 2], data[off + 1], data[off + 0], data[off + 3]
end

local function near(r, g, b, want, tol)
    return math.abs(r - want.r) <= (tol or 3)
       and math.abs(g - want.g) <= (tol or 3)
       and math.abs(b - want.b) <= (tol or 3)
end

local function screen_pixel(x, y)
    -- raw pointer surface; screen[1].content returns an lgi record, not FFI-readable
    local raw = root.content()
    if not raw then return nil, nil, nil end
    return pixel_rgb(raw, math.max(0, math.floor(x)), math.max(0, math.floor(y)))
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
        -- Give the spawned client rounded corners via the per-client
        -- property (also exercises the theme resolver through the getter).
        local con = client.connect_signal
        con("request::manage", function(c)
            if c.class == APP_ID or c.instance == APP_ID then
                c.corner_radius = { radius = MASK_RADIUS, enabled = true }
                c.border_width = 0
            end
        end)

        local pid = awful.spawn(BINARY)
        assert(type(pid) == "number" and pid > 0, "failed to spawn pattern client")
        table.insert(pids, pid)

        local c = async.wait_for_client(APP_ID, 5)
        assert(c, "pattern client never appeared")

        -- Assert the property round-trips.
        local cfg = c.corner_radius
        assert(type(cfg) == "table", "c.corner_radius should be a table, got " .. type(cfg))
        assert(cfg.radius == MASK_RADIUS, "c.corner_radius.radius should be "
            .. MASK_RADIUS .. ", got " .. tostring(cfg.radius))

        local g0 = c:geometry()
        c.floating = true
        c:geometry { x = 173, y = 109, width = math.max(g0.width, 300), height = math.max(g0.height, 240) }
        local g = c:geometry()
        async.sleep(0.2)

        wait_for_painted(c, 5)
        async.sleep(0.2)   -- let apply_geometry_to_wlroots place the masks

        local gx, gy = g.x, g.y

        -- The background just outside the frame is what a transparent
        -- corner must reveal (wallpaper/root color at that spot).
        local br, bg, bb = screen_pixel(gx - 2, gy - 2)
        local BACKGROUND = { r = br, g = bg, b = bb }
        assert(not near(br, bg, bb, { r = 255, g = 0, b = 0 }),
            "background sample must not be red (would make the test meaningless)")

        -- (1) The cut corner must be transparent: it shows the background,
        --     not the client's red.
        local r, gg, b = screen_pixel(gx + 1, gy + 1)
        io.stderr:write(("[rounded] step1 corner @(%d,%d) rgb(%d,%d,%d) background rgb(%d,%d,%d)\n")
            :format(gx, gy, r, gg, b, br, bg, bb))
        assert(near(r, gg, b, BACKGROUND, 6),
            string.format("corner pixel should reveal the background rgb(%d,%d,%d), got rgb(%d,%d,%d) @ (%d,%d)",
                br, bg, bb, r, gg, b, gx + 1, gy + 1))

        -- (2) Just inside the arc the quadrant still shows.
        r, gg, b = screen_pixel(gx + MASK_RADIUS + 4, gy + MASK_RADIUS + 4)
        io.stderr:write(("[rounded] step2 inside-arc rgb(%d,%d,%d)\n"):format(r, gg, b))
        assert(near(r, gg, b, { r = 255, g = 0, b = 0 }),
            string.format("inside-arc pixel should be red, got rgb(%d,%d,%d)", r, gg, b))

        -- (3) Uncut interior stays red.
        r, gg, b = screen_pixel(gx + 60, gy + 60)
        io.stderr:write(("[rounded] step3 interior rgb(%d,%d,%d)\n"):format(r, gg, b))
        assert(near(r, gg, b, { r = 255, g = 0, b = 0 }),
            string.format("interior pixel should be red, got rgb(%d,%d,%d)", r, gg, b))

        -- (4) Fullscreen disables the crop: corner pixel returns to client red.
        c.fullscreen = true
        async.sleep(0.3)
        wait_for_painted(c, 5)
        r, gg, b = screen_pixel(1, 1)
        assert(near(r, gg, b, { r = 255, g = 0, b = 0 }),
            string.format("fullscreen corner pixel should be red (no crop), got rgb(%d,%d,%d)", r, gg, b))

        -- (5) Leaving fullscreen restores the crop.
        c.fullscreen = false
        async.sleep(0.3)
        wait_for_painted(c, 5)
        g = c:geometry()
        r, gg, b = screen_pixel(g.x + 1, g.y + 1)
        assert(near(r, gg, b, BACKGROUND, 6),
            string.format("corner pixel should reveal the background again, got rgb(%d,%d,%d)", r, gg, b))

        io.stderr:write("[rounded] radius=" .. MASK_RADIUS
            .. " corner=" .. string.format("rgb(%d,%d,%d)", r, gg, b)
            .. " interior=red fullscreen=uncut restored=ok\n")
    end)

    for _, pid in ipairs(pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    for _, c in ipairs(client.get()) do c:kill() end
    async.wait_for_no_clients(3)

    if not ok then
        runner.done("test-rounded-corners: " .. tostring(err))
    else
        runner.done()
    end
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80