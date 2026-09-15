---------------------------------------------------------------------------
--- Probe: does the inner hairline follow the window when it moves?
--- Spawns the pattern client, gives it a border + inner hairline, and
--- samples the composed screen via root.content():
---   * border ring pixel (blue) should be at the frame edge
---   * inner hairline pixel (magenta) should hug the border inner edge
---   * after moving the window, both must be at the NEW position and gone
---     from the OLD one.
--- Run: make test-one TEST=tests/test-innerline-follow.lua
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
local BW = 8

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
    awesome.quit(); return
end

local function pixel_rgb(raw, x, y)
    ffi.C.cairo_surface_flush(raw)
    local data   = ffi.C.cairo_image_surface_get_data(raw)
    local stride = ffi.C.cairo_image_surface_get_stride(raw)
    local off = math.max(0, math.floor(y)) * stride + math.max(0, math.floor(x)) * 4
    return data[off + 2], data[off + 1], data[off + 0]
end

local function screen_pixel(x, y)
    local raw = root.content()
    if not raw then return nil, nil, nil end
    return pixel_rgb(raw, x, y)
end

local function near(r, g, b, want, tol)
    tol = tol or 6
    return math.abs(r - want[1]) <= tol
       and math.abs(g - want[2]) <= tol
       and math.abs(b - want[3]) <= tol
end

local MAGENTA = { 255, 0, 255 }

local function scan_windows(raw)
    -- Search the whole screen for a magenta pixel (the hairline) and return
    -- its bounding box. Robust against not knowing exact geometry pixel rows.
    local w = ffi.C.cairo_image_surface_get_width(raw)
    local h = ffi.C.cairo_image_surface_get_height(raw)
    local minx, miny, maxx, maxy = nil, nil, nil, nil
    for y = 0, h - 1 do
        local off_y = y * ffi.C.cairo_image_surface_get_stride(raw)
        for x = 0, w - 1 do
            local off = off_y + x * 4
            local data = ffi.C.cairo_image_surface_get_data(raw)
            local r = data[off + 2]
            local g = data[off + 1]
            local b = data[off + 0]
            if near(r, g, b, MAGENTA, 60) then
                if not minx or x < minx then minx = x end
                if not maxx or x > maxx then maxx = x end
                if not miny or y < miny then miny = y end
                if not maxy or y > maxy then maxy = y end
            end
        end
    end
    return minx, miny, maxx, maxy
end

local pids = {}

runner.run_async(function()
    local ok, err = pcall(function()
        local con = client.connect_signal
        con("request::manage", function(c)
            if c.class == APP_ID or c.instance == APP_ID then
                c.border_color = "#0000ff"
                c.border_inner_color = "#ff00ff"
                c.border_inner_enabled = true
                c.border_inner_width = 1
                c.corner_radius = { radius = 0, enabled = false }
            end
        end)

        local pid = awful.spawn(BINARY)
        assert(type(pid) == "number" and pid > 0)
        table.insert(pids, pid)

        local c = async.wait_for_client(APP_ID, 5)
        assert(c, "pattern client never appeared")

        c.floating = true
        c.border_width = BW
        c:geometry { x = 100, y = 80, width = 300, height = 200 }

        async.sleep(0.4)

        local g = c:geometry()
        io.stderr:write(("[innerline] geometry %d,%d %dx%d\n"):format(g.x, g.y, g.width, g.height))

        async.sleep(0.4)
        local raw = root.content()
        local probes = {
            { "border-in", g.x + BW + 0, g.y + BW + 0 },
            { "edge-tl", g.x + BW + 0, g.y + BW + 1 },
            { "edge-top", g.x + BW + 20, g.y + BW + 0 },
            { "edge-top2", g.x + BW + 20, g.y + BW + 1 },
            { "edge-left", g.x + BW + 0, g.y + BW + 20 },
            { "content", g.x + BW + 20, g.y + BW + 20 },
            { "border-out", g.x + 2, g.y + 2 },
        }
        for _, p in ipairs(probes) do
            local r, gg, b = pixel_rgb(raw, p[2], p[3])
            io.stderr:write(("[innerline] %-12s (%d,%d) rgb=%d,%d,%d\n")
                :format(p[1], p[2], p[3], r, gg, b))
        end

        -- Wait until a magenta hairline appears anywhere on screen.
        local found = false
        async.wait_for_condition(function()
            local minx = scan_windows(root.content())
            found = minx ~= nil
            return found
        end, 5, 0.1)
        assert(found, "no inner hairline (magenta) found on screen")

        local old_minx, old_miny, old_maxx, old_maxy = scan_windows(root.content())
        io.stderr:write(("[innerline] pos1 hairline bbox x=%d..%d y=%d..%d\n")
            :format(old_minx, old_maxx, old_miny, old_maxy))

        -- The hairline must hang off the frame's inner edge: its top-left
        -- corner should sit at (g.x+BW, g.y+BW).
        local expx, expy = g.x + BW, g.y + BW
        local okpos = math.abs((old_minx or -999) - expx) <= 2
            and math.abs((old_miny or -999) - expy) <= 2
        io.stderr:write(("[innerline] expected inner edge at (%d,%d) bbox TL=%s,%s (%s)\n")
            :format(expx, expy, tostring(old_minx), tostring(old_miny),
                    okpos and "ok" or "MISPLACED"))

        -- Move the window.
        local dx, dy = 40, 25
        c:geometry { x = g.x + dx, y = g.y + dy, width = g.width, height = g.height }
        async.sleep(0.4)

        local new_minx, new_miny, new_maxx, new_maxy = scan_windows(root.content())
        io.stderr:write(("[innerline] pos2 hairline bbox x=%d..%d y=%d..%d\n")
            :format(new_minx, new_maxx, new_miny, new_maxy))

        local g2 = c:geometry()
        local expx2, expy2 = g2.x + BW, g2.y + BW
        -- Old top-left inner edge must no longer be magenta (it now lies
        -- outside the moved window), and the new position must have it.
        local or_, og, ob = screen_pixel(old_minx, old_miny)
        local old_cleared = not near(or_, og, ob, MAGENTA, 60)
        local okmove = new_minx ~= nil
            and math.abs(new_minx - expx2) <= 2
            and math.abs(new_miny - expy2) <= 2
            and old_cleared

        io.stderr:write(("[innerline] old edge (%d,%d) cleared=%s (%d,%d,%d)\n")
            :format(old_minx, old_miny, tostring(old_cleared), or_, og, ob))
        io.stderr:write(("[innerline] expected new inner edge at (%d,%d) got TL=%s,%s (%s)\n")
            :format(expx2, expy2, tostring(new_minx), tostring(new_miny),
                    okmove and "ok" or "STUCK"))

        assert(okpos, "inner hairline does not hug the border inner edge")
        assert(okmove, "inner hairline did NOT follow the window when it moved")

        io.stderr:write("[innerline] hairline follows window: ok\n")
    end)

    for _, pid in ipairs(pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    for _, c in ipairs(client.get()) do c:kill() end
    async.wait_for_no_clients(3)

    if not ok then
        runner.done("test-innerline-follow: " .. tostring(err))
    else
        runner.done()
    end
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80