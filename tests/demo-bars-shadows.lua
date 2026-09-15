---------------------------------------------------------------------------
--- Visual showcase + verification: 4 bars/docks and static widgets with
--- different shadow and rounded-corner configs.
---
--- Uses the config knobs documented in docs/features_test.md:
---   * rounded corners for drawins/wiboxes/the wibar: `corner_radius`
---     (§1.4) including the per-corner table form (§1.5)
---   * compositor drop shadows for drawins/wiboxes: `shadow` (§2.3),
---     following the object's rounded corners (§2.4)
---   * the inner hairline (§3.4) on one static widget
---
--- Every bar/dock/widget is captured with grim (wlr-screencopy) and saved
--- under docs/, AND verified pixel-by-pixel through
--- root.content() so a "not rendering" regression (missing bar, missing
--- shadow, square corners, un-cut corners) fails the test instead of
--- silently producing a broken screenshot. root.content() composites the
--- scene graph including the shadow's solid rect fills.
---
--- Run (needs grim on PATH for the screenshots; pixel checks run regardless):
---   nix-shell -p luajit luajitPackages.lgi bc grim --run '
---     cd /home/humbe/somewm && VERBOSE=1 TEST_TIMEOUT=90 HEADLESS=1 \
---       SOMEWM=./build-test/somewm SOMEWM_CLIENT=./build-test/somewm-client \
---       ./tests/run-integration.sh tests/demo-bars-shadows.lua'
---------------------------------------------------------------------------

local awful     = require("awful")
local async     = require("_async")
local runner    = require("_runner")
local wibox     = require("wibox")
local gears     = require("gears")

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
]]

local SHOTS_DIR = "docs"

local function grim_available()
    local ok = os.execute("command -v grim >/dev/null 2>&1")
    return ok == 0 or ok == true
end

local function pixel_rgb(raw, x, y)
    ffi.C.cairo_surface_flush(raw)
    local data   = ffi.C.cairo_image_surface_get_data(raw)
    local stride = ffi.C.cairo_image_surface_get_stride(raw)
    local off = math.max(0, math.floor(y)) * stride + math.max(0, math.floor(x)) * 4
    return data[off + 2], data[off + 1], data[off + 0], data[off + 3]
end

local function sample(x, y)
    local raw = root.content()
    if not raw then return nil, nil, nil, nil end
    return pixel_rgb(raw, x, y)
end

local function near(r, g, b, want, tol)
    tol = tol or 30
    return math.abs(r - want[1]) <= tol
       and math.abs(g - want[2]) <= tol
       and math.abs(b - want[3]) <= tol
end

-- Expected shadow color at full opacity over the light wallpaper
-- (the test uses #e7e7e7 as background):
--   wallpaper * (1 - opacity)
local function shadow_expected(opacity)
    return { math.floor(231 * (1 - opacity) + 0.5),
             math.floor(231 * (1 - opacity) + 0.5),
             math.floor(231 * (1 - opacity) + 0.5) }
end

local function capture(path)
    os.remove(path)
    awful.spawn.with_shell("grim " .. path)
    local size = 0
    async.wait_for_condition(function()
        local f = io.open(path, "r")
        if not f then return false end
        size = f:seek("end")
        f:close()
        return size > 2000
    end, 10, 0.1)
    async.sleep(0.3)
    io.stderr:write("[bars] saved " .. path .. " (" .. size .. " bytes)\n")
end

-- Verify one bar/dock/widget against root.content():
--   obj  : the wibox/wibar
--   spec : {
--     bg        = {r,g,b}                    -- expected interior color
--     rounded   = {tl=bool,tr=bool,bl=bool,br=bool}
--     shadow    = { point = {x,y}, opacity = number }  -- pixel inside the
--                       solid shadow rect; opacity -> expected color
--     label     = string
--   }
local function verify_object(obj, spec)
    local g = obj:geometry()
    local gx, gy, w, h = g.x, g.y, g.width, g.height
    assert(w > 0 and h > 0, spec.label .. ": empty geometry " .. tostring(w) .. "x" .. tostring(h))

    -- 1. Interior must render the bar color (catches "not rendering").
    local r, gg, b = sample(gx + math.floor(w / 2), gy + math.floor(h / 2))
    assert(near(r, gg, b, spec.bg),
        string.format("%s: interior should render rgb(%d,%d,%d), got rgb(%d,%d,%d) @(%d,%d)",
            spec.label, spec.bg[1], spec.bg[2], spec.bg[3], r, gg, b, gx + w / 2, gy + h / 2))

    -- 2. The drop shadow must be present in its solid region at (close to)
    --    full opacity (catches a shadow that is not rendering).
    if spec.shadow then
        local px, py = spec.shadow.point[1], spec.shadow.point[2]
        local want = shadow_expected(spec.shadow.opacity)
        r, gg, b = sample(px, py)
        assert(near(r, gg, b, want, 25),
            string.format("%s: shadow missing at (%d,%d): got rgb(%d,%d,%d), expected ~rgb(%d,%d,%d)",
                spec.label, px, py, r, gg, b, want[1], want[2], want[3]))
    end

    -- 3/4. Rounded corners must be cut (corner pixel != bar color), square
    --    corners must be square (corner pixel == bar color).
    local corners = {
        { "tl", gx + 1, gy + 1 },
        { "tr", gx + w - 1, gy + 1 },
        { "bl", gx + 1, gy + h - 1 },
        { "br", gx + w - 1, gy + h - 1 },
    }
    for _, c in ipairs(corners) do
        r, gg, b = sample(c[2], c[3])
        if spec.rounded[c[1]] then
            assert(not near(r, gg, b, spec.bg, 12),
                string.format("%s: %s corner should be rounded (cut), got bar color rgb(%d,%d,%d)",
                    spec.label, c[1], r, gg, b))
        else
            assert(near(r, gg, b, spec.bg, 12),
                string.format("%s: %s corner should be square, got rgb(%d,%d,%d) expected rgb(%d,%d,%d)",
                    spec.label, c[1], r, gg, b, spec.bg[1], spec.bg[2], spec.bg[3]))
        end
    end

    io.stderr:write("[bars] verified " .. spec.label .. " @ " .. gx .. "," .. gy
        .. " " .. w .. "x" .. h .. "\n")
end

local bars    = {}   -- the 4 bars, in order
local widgets = {}   -- the static widgets, in order

runner.run_async(function()
    local ok, err = pcall(function()
        pcall(function() require("gears.wallpaper").set("#e7e7e7") end)
        async.sleep(0.3)

        local sgeo = awful.screen.focused().geometry
        local W, H = sgeo.width, sgeo.height
        io.stderr:write(("[bars] screen %dx%d\n"):format(W, H))

        -- Bar 1: top dock (wibar), rounded top corners, crisp downward shadow.
        local bar1 = awful.wibar {
            position = "top",
            screen   = awful.screen.focused(),
            height   = 34,
            bg       = "#2e3440",
            border_width = 0,
        }
        bar1.corner_radius = { radius = { 16, 16, 0, 0 }, enabled = true }
        bar1.shadow = {
            enabled = true, radius = 8, offset_x = 0, offset_y = 6,
            spread = 6, opacity = 0.5, color = "#000000", follow_corners = true,
        }
        table.insert(bars, bar1)

        -- Bar 2: left dock (floating), all corners rounded, soft spread shadow.
        local bar2 = wibox {
            x = 16, y = 110, width = 220, height = 380,
            bg = "#4c566a", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        bar2.corner_radius = { radius = 12, enabled = true }
        bar2.shadow = {
            enabled = true, radius = 18, offset_x = 6, offset_y = 6,
            spread = 14, opacity = 0.35, color = "#000000", follow_corners = true,
        }
        table.insert(bars, bar2)

        -- Bar 3: bottom "waybar" pill, fully rounded, upward shadow.
        local bar3 = wibox {
            x = math.floor((W - 520) / 2), y = H - 40 - 22, width = 520, height = 40,
            bg = "#3b4252", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        bar3.corner_radius = { radius = 20, enabled = true }
        bar3.shadow = {
            enabled = true, radius = 10, offset_x = 0, offset_y = -5,
            spread = 4, opacity = 0.45, color = "#000000", follow_corners = true,
        }
        table.insert(bars, bar3)

        -- Bar 4: right dock, bottom corners only rounded, crisp inward shadow.
        local bar4 = wibox {
            x = W - 260 - 16, y = H - 340 - 16, width = 260, height = 340,
            bg = "#5e81ac", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        bar4.corner_radius = { radius = { 0, 0, 14, 14 }, enabled = true }
        bar4.shadow = {
            enabled = true, radius = 7, offset_x = -5, offset_y = 0,
            spread = 6, opacity = 0.6, color = "#000000", follow_corners = true,
        }
        table.insert(bars, bar4)

        -- Static widgets (row below the left dock), each with a different
        -- shadow/rounded-corner combination.
        local widgetA = wibox {
            x = 40, y = 530, width = 120, height = 64,
            bg = "#bf616a", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        widgetA.corner_radius = { radius = 8, enabled = true }
        widgetA.shadow = {
            enabled = true, radius = 5, offset_x = 2, offset_y = 4,
            spread = 4, opacity = 0.6, color = "#000000", follow_corners = true,
        }
        table.insert(widgets, widgetA)

        -- Lua shape owns the corners here; the shadow stays square (the
        -- shape owns its own geometry, see docs/features_test.md §2.1).
        local widgetB = wibox {
            x = 180, y = 530, width = 140, height = 64,
            bg = "#d08770", border_width = 2, border_color = "#5b3a2e",
            visible = true, screen = awful.screen.focused(),
            shape = function(cr, w, h) gears.shape.rounded_rect(cr, w, h, 12) end,
        }
        widgetB.shadow = {
            enabled = true, radius = 6, offset_x = 0, offset_y = 5,
            spread = 6, opacity = 0.5, color = "#000000",
        }
        table.insert(widgets, widgetB)

        -- No shadow at all: only the top corners are rounded.
        local widgetC = wibox {
            x = 340, y = 530, width = 120, height = 64,
            bg = "#a3be8c", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        widgetC.corner_radius = { radius = { 12, 12, 0, 0 }, enabled = true }
        widgetC.shadow = false
        table.insert(widgets, widgetC)

        -- Fully rounded + soft shadow + macOS inner hairline (§3.4).
        local widgetD = wibox {
            x = 480, y = 530, width = 140, height = 64,
            bg = "#ebcb8b", border_width = 0, visible = true,
            screen = awful.screen.focused(),
        }
        widgetD.corner_radius = { radius = 16, enabled = true }
        widgetD.shadow = {
            enabled = true, radius = 12, offset_x = 0, offset_y = 8,
            spread = 10, opacity = 0.4, color = "#000000", follow_corners = true,
        }
        widgetD:set_border_inner_enabled(true)
        widgetD:set_border_inner_width(2)
        widgetD:set_border_inner_color("rgba(255, 105, 180, 0.5)")
        table.insert(widgets, widgetD)

        -- Let every shadow be created and laid out by the compositor.
        async.sleep(1.0)

        local all = {}
        for _, b in ipairs(bars) do all[#all + 1] = b end
        for _, wd in ipairs(widgets) do all[#all + 1] = wd end

        local function show_only(list)
            for _, wd in ipairs(all) do wd.visible = false end
            for _, wd in ipairs(list) do wd.visible = true end
        end
        local function show_all()
            for _, wd in ipairs(all) do wd.visible = true end
        end

        -- Expected solid-shadow sample points (frame-grown-by-spread rect,
        -- translated by the offset; full opacity inside it).
        local bar_specs = {
            {
                label = "bar1-top-dock",
                bg = { 0x2e, 0x34, 0x40 },
                rounded = { tl = true, tr = true, bl = false, br = false },
                shadow = { point = { 640, 34 + 4 }, opacity = 0.5 },
            },
            {
                label = "bar2-left-dock",
                bg = { 0x4c, 0x56, 0x6a },
                rounded = { tl = true, tr = true, bl = true, br = true },
                shadow = { point = { 16 + 110, 110 + 380 + 12 }, opacity = 0.35 },
            },
            {
                label = "bar3-bottom-waybar",
                bg = { 0x3b, 0x42, 0x52 },
                rounded = { tl = true, tr = true, bl = true, br = true },
                shadow = { point = { 640, (H - 40 - 22) - 5 }, opacity = 0.45 },
            },
            {
                label = "bar4-right-dock",
                bg = { 0x5e, 0x81, 0xac },
                rounded = { tl = false, tr = false, bl = true, br = true },
                shadow = { point = { W - 260 - 16 - 9, 534 }, opacity = 0.6 },
            },
        }

        local widget_specs = {
            {
                label = "widgetA-crisp",
                bg = { 0xbf, 0x61, 0x6a },
                rounded = { tl = true, tr = true, bl = true, br = true },
                shadow = { point = { 100, 530 + 64 + 3 }, opacity = 0.6 },
            },
            {
                label = "widgetB-shape-square-shadow",
                bg = { 0xd0, 0x87, 0x70 },
                rounded = { tl = true, tr = true, bl = true, br = true },
                shadow = { point = { 250, 530 + 64 + 5 }, opacity = 0.5 },
            },
            {
                label = "widgetC-no-shadow",
                bg = { 0xa3, 0xbe, 0x8c },
                rounded = { tl = true, tr = true, bl = false, br = false },
            },
            {
                label = "widgetD-hairline",
                bg = { 0xeb, 0xcb, 0x8b },
                rounded = { tl = true, tr = true, bl = true, br = true },
                shadow = { point = { 550, 530 + 64 + 7 }, opacity = 0.4 },
            },
        }

        -- Property round-trips (docs/features_test.md §2.3 / §1.4).
        local s1 = bar1.shadow
        assert(type(s1) == "table" and s1.enabled == true, "bar1.shadow should be enabled")
        assert(s1.follow_corners == true, "bar1.shadow.follow_corners should be true")
        local c1 = bar1.corner_radius
        assert(type(c1) == "table", "bar1.corner_radius should be a table")
        local cr = c1.corner_radii or (c1.radius and type(c1.radius) == "table" and c1.radius)
        assert(cr and cr[1] == 16 and cr[2] == 16 and cr[3] == 0 and cr[4] == 0,
            "bar1 per-corner radii should round-trip {16,16,0,0}")
        assert(widgetC.shadow == false, "widgetC.shadow should be false")

        -- Capture the overview first, then one focused shot per object.
        show_all()
        async.sleep(0.5)
        if grim_available() then capture(SHOTS_DIR .. "/bars-overview.png") end

        for i, spec in ipairs(bar_specs) do
            show_only({ bars[i] })
            async.sleep(0.5)
            if grim_available() then
                capture(string.format("%s/bars-%d-%s.png", SHOTS_DIR, i, spec.label))
            end
            verify_object(bars[i], spec)
        end

        for i, spec in ipairs(widget_specs) do
            show_only({ widgets[i] })
            async.sleep(0.5)
            if grim_available() then
                capture(string.format("%s/bars-widget-%s.png", SHOTS_DIR, spec.label))
            end
            verify_object(widgets[i], spec)
        end

        io.stderr:write(("[bars] all captures + pixel checks written under %s/\n"):format(SHOTS_DIR))
    end)

    for _, b in ipairs(bars) do b.visible = false end
    for _, wd in ipairs(widgets) do wd.visible = false end

    if not ok then
        runner.done("demo-bars-shadows: " .. tostring(err))
    else
        runner.done()
    end
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80