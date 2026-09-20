---------------------------------------------------------------------------
-- Pixel-content test for client:dominant_color().
--
-- Verifies the compositor-side dominant-color sampler against the same
-- 4-quadrant pattern clients used by the c.content tests:
--
--   * rows = 12 on the quadrant pattern returns red or green (the top two
--     quadrants), never blue or yellow, with share ~0.5.
--   * Parity: for rows = 12, the returned share matches, within 1%, that
--     color's fraction in the top 12 rows of c.content counted via FFI.
--   * rows = 0 (whole content) returns one of the four colors with
--     share ~0.25.
--
-- Runs for both the SHM client (test-content-pattern-client) and the DMA-BUF
-- client (test-dmabuf-pattern-client). Same skip conditions as the existing
-- content tests: non-LuaJIT, missing client binary, or no DRM render node.
--
-- Run: make test-one TEST=tests/test-client-dominant-color.lua
--------------------------------------------------------------------------

local awful  = require("awful")
local runner = require("_runner")
local async  = require("_async")

local ok_ffi, ffi = pcall(require, "ffi")
if not ok_ffi then
    io.stderr:write("SKIP: ffi not available (non-LuaJIT build); parity check needs FFI\n")
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

local function find_binary(prefix)
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    for _, candidate in ipairs({
        build_dir .. "/" .. prefix,
        "./build/" .. prefix,
        "./build-test/" .. prefix,
    }) do
        local f = io.open(candidate, "r")
        if f then f:close(); return candidate end
    end
    return nil
end

-- Count the fraction of `hex` among the top `rows` logical rows of a
-- c.content surface via FFI (same step the method uses: step_x=2, step_y=1).
local function top_rows_share(raw_surface, rows, hex)
    ffi.C.cairo_surface_flush(raw_surface)
    local data   = ffi.C.cairo_image_surface_get_data(raw_surface)
    local stride = ffi.C.cairo_image_surface_get_stride(raw_surface)
    local w      = ffi.C.cairo_image_surface_get_width(raw_surface)
    local h      = ffi.C.cairo_image_surface_get_height(raw_surface)
    local match, total = 0, 0
    for y = 0, math.min(rows, h) - 1 do
        for x = 0, w - 1, 2 do
            local off = y * stride + x * 4
            local hex_ = string.format("#%02x%02x%02x",
                data[off + 2], data[off + 1], data[off + 0])
            total = total + 1
            if hex_ == hex then match = match + 1 end
        end
    end
    return match / total
end

local function dominant(r, g, b)
    if r > 200 and g <  80 and b <  80 then return "red"    end
    if g > 200 and r <  80 and b <  80 then return "green"  end
    if b > 200 and r <  80 and g <  80 then return "blue"   end
    if r > 200 and g > 200 and b <  80 then return "yellow" end
    return string.format("rgb(%d,%d,%d)", r, g, b)
end

local function assert_quadrant_color(hex, context)
    local r, g, b = tonumber(hex:sub(2, 3), 16),
                    tonumber(hex:sub(4, 5), 16),
                    tonumber(hex:sub(6, 7), 16)
    local name = dominant(r, g, b)
    assert(name == "red" or name == "green",
        context .. " rows=12 should be red or green, got " .. name)
    return name, hex
end

runner.run_async(function()
    local cases = {}

    local pattern_bin = find_binary("test-content-pattern-client")
    if pattern_bin then
        table.insert(cases, {
            bin = pattern_bin,
            app = "content_pattern_test",
            label = "SHM",
        })
    else
        io.stderr:write("SKIP SHM case: test-content-pattern-client not built\n")
    end

    local dmabuf_bin = find_binary("test-dmabuf-pattern-client")
    if dmabuf_bin then
        table.insert(cases, {
            bin = dmabuf_bin,
            app = "dmabuf_pattern_test",
            label = "DMA-BUF",
        })
    else
        io.stderr:write("SKIP DMA-BUF case: test-dmabuf-pattern-client not built\n")
    end

    assert(#cases > 0, "no client binaries available to test against")

    local s = screen[1]
    s.scale = 1.0
    async.sleep(0.05)

    for _, case in ipairs(cases) do
        local pid = awful.spawn({case.bin})
        assert(type(pid) == "number" and pid > 0,
            "Failed to spawn " .. case.bin)
        local c = async.wait_for_client(case.app, 5)
        if not c then
            os.execute("kill -9 " .. pid .. " 2>/dev/null")
            io.stderr:write("SKIP " .. case.label .. ": client never appeared (no DRM render node?)\n")
            runner.done(); return
        end
        c.floating = true
        c:geometry { x = 173, y = 109, width = c:geometry().width, height = c:geometry().height }
        async.sleep(0.1)

        -- rows = 12: top quadrants only (red | green), never blue/yellow.
        local hex12, share12 = c:dominant_color { rows = 12 }
        assert(hex12, case.label .. " rows=12 returned no color")
        assert(type(share12) == "number" and share12 > 0 and share12 <= 1,
            case.label .. " rows=12 bad share: " .. tostring(share12))
        assert_quadrant_color(hex12, case.label)
        assert(share12 > 0.49 and share12 < 0.51,
            case.label .. " rows=12 share should be ~0.5, got " .. tostring(share12))

        -- Parity: c.content top-12 fraction of the same color, within 1%.
        local raw = c.content
        assert(raw, case.label .. " c.content returned nil for parity check")
        local ffi_share = top_rows_share(raw, 12, hex12)
        assert(math.abs(share12 - ffi_share) <= 0.01,
            string.format("%s rows=12 share %.4f vs c.content FFI %.4f (delta %.4f > 1%%)",
                case.label, share12, ffi_share, math.abs(share12 - ffi_share)))

        -- rows = 0: whole content, one of the four quadrants, share ~0.25.
        local hex0, share0 = c:dominant_color { rows = 0 }
        assert(hex0, case.label .. " rows=0 returned no color")
        assert(type(share0) == "number" and share0 > 0 and share0 <= 1,
            case.label .. " rows=0 bad share: " .. tostring(share0))
        local r0, g0, b0 = tonumber(hex0:sub(2, 3), 16),
                           tonumber(hex0:sub(4, 5), 16),
                           tonumber(hex0:sub(6, 7), 16)
        local name0 = dominant(r0, g0, b0)
        assert(name0 == "red" or name0 == "green" or
               name0 == "blue" or name0 == "yellow",
            case.label .. " rows=0 should be one of the four colors, got " .. name0)
        assert(share0 > 0.24 and share0 < 0.26,
            case.label .. " rows=0 share should be ~0.25, got " .. tostring(share0))

        io.stderr:write(string.format(
            "[dominant-color] %s rows=12=%s share=%.4f ffi=%.4f rows=0=%s share=%.4f\n",
            case.label, hex12, share12, ffi_share, hex0, share0))

        c:kill()
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
        async.wait_for_no_clients(3)
    end

    runner.done()
end)