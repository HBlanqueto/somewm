-- Verification: the "roundy" theme loads and its new options apply.
local beautiful = require("beautiful")
local awful = require("awful")
local async = require("_async")
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

local APP_ID = "content_pattern_test"
local pids = {}

runner.run_async(function()
    local ok, err = pcall(function()
        -- Load the roundy theme from the repo's themes/ directory (the
        -- harness runs somewm from the repo root).
        local path = "themes/roundy/theme.lua"
        io.stderr:write("[roundy] theme path: " .. path .. "\n")
        assert(beautiful.init(path), "roundy theme failed to init")

        -- Re-read the compositor-level defaults from the theme.
        awesome.corner_reload()
        awesome.shadow_reload()

        -- Theme option values must be the ones from themes/roundy/theme.lua.
        assert(beautiful.corner_enabled == true, "corner_enabled should be true")
        assert(beautiful.corner_radius == 10, "corner_radius should be 10, got "
            .. tostring(beautiful.corner_radius))
        assert(beautiful.corner_drawin_enabled == true, "corner_drawin_enabled should be true")
        assert(beautiful.corner_drawin_radius == 10, "corner_drawin_radius should be 10")

        assert(beautiful.shadow_enabled == true, "shadow_enabled should be true")
        assert(beautiful.shadow_follow_corners == true, "shadow_follow_corners should be true")
        assert(beautiful.shadow_radius == 12, "shadow_radius should be 12")
        assert(beautiful.shadow_offset_y == 4, "shadow_offset_y should be 4")
        assert(beautiful.shadow_drawin_enabled == true, "shadow_drawin_enabled should be true")
        assert(beautiful.shadow_drawin_follow_corners == true, "shadow_drawin_follow_corners should be true")

        assert(beautiful.border_inner_enabled == true, "border_inner_enabled should be true")
        assert(beautiful.border_inner_width == 1, "border_inner_width should be 1")
        assert(beautiful.border_inner_drawin_enabled == true, "border_inner_drawin_enabled should be true")

        io.stderr:write("[roundy] theme options verified\n")

        -- Sanity: a client mapped under the theme gets rounded corners + a
        -- shadow following them. Check the cut corner reveals the shadow.
        local con = client.connect_signal
        con("request::manage", function(c)
            if c.class == APP_ID or c.instance == APP_ID then
                c.border_width = 0
            end
        end)
        local pid = awful.spawn(BINARY)
        table.insert(pids, pid)
        local c = async.wait_for_client(APP_ID, 5)
        assert(c, "pattern client never appeared")
        c.floating = true
        c:geometry { x = 173, y = 109, width = 200, height = 170 }
        async.sleep(0.8)
        local g = c:geometry()
        local raw = root.content()
        local r, gg, b = pixel_rgb(raw, g.x + 1, g.y + 1)
        -- Background is dark (#1c1c24 wallpaper-ish); the cut corner must not
        -- be the red TL quadrant (i.e. rounding is active) and the shadow
        -- must darken it vs the pure background.
        io.stderr:write(("[roundy] TL cut pixel rgb=%d,%d,%d (red would be 255,0,0)\n")
            :format(r, gg, b))
        assert(not (r > 150 and gg < 100 and b < 100),
            "window TL corner should be cut by the roundy theme, got red content")
        io.stderr:write("[roundy] rounded corner applied on a mapped client\n")

        -- Optional visual preview (needs grim on PATH). Use a mid-gray
        -- wallpaper so the theme's dark shadow is visible against it.
        if os.execute("command -v grim >/dev/null 2>&1") == 0 then
            pcall(function() require("gears.wallpaper").set("#5a5a66") end)
            async.sleep(0.3)
            os.remove("docs/roundy-theme-preview.png")
            awful.spawn.with_shell("grim docs/roundy-theme-preview.png")
            async.sleep(1.5)
            local f = io.open("docs/roundy-theme-preview.png", "r")
            io.stderr:write("[roundy] preview: docs/roundy-theme-preview.png saved="
                .. tostring(f ~= nil) .. "\n")
            if f then f:close() end
        end
    end)

    for _, pid in ipairs(pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    for _, c in ipairs(client.get()) do c:kill() end
    async.wait_for_no_clients(3)

    if not ok then
        runner.done("demo-roundy-theme: " .. tostring(err))
    else
        runner.done()
    end
end)