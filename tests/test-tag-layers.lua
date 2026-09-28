-- Test: tag.layers — layer-shell surfaces appear/disappear and slide with a tag.
--
-- tag.layers is a list-of-strings property on tag (following role /
-- client_policy). A layer surface whose namespace is in a tag's layers list is
-- shown only while that tag is selected on its output, hidden otherwise, and
-- slides with that tag during a workspace slide. Zero window clients: the only
-- content is the layer surface and its tag.
--
-- A single gray 100x100 layer surface (namespace NS) is anchored top,left at
-- x=0 and sampled at (0,90): gray while its tag is selected, background
-- otherwise. Direction is controlled by tag order so the incoming tag slides in
-- from the right (offset > 0), which makes a mid-slide sample at (0,90) read
-- background while the surface is still off its anchor.

local runner = require("_runner")
local async = require("_async")
local awful = require("awful")

if not os.execute("command -v grim >/dev/null 2>&1") then
    io.stderr:write("SKIP: grim not available\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local slide = _G.slide
if not (slide and slide.set_duration and slide.active) then
    io.stderr:write("SKIP: compositor slide driver missing\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local NS = "tag-layer-test"
local X = 0
local Y = 90
local LONG = 2.0 -- long enough to catch a mid-slide frame

local function find_layer_client()
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    local candidates = {
        build_dir .. "/test-layer-client",
        "./build-test/test-layer-client",
        "./build/test-layer-client",
    }
    for _, p in ipairs(candidates) do
        local f = io.open(p, "r")
        if f then f:close() return p end
    end
    return nil
end

-- Minimal P6 PPM reader: "P6\n<w> <h>\n255\n" then raw RGB.
local function load_ppm(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local magic = f:read(2)
    assert(magic == "P6", "not a P6 ppm: " .. magic)
    local w, h = f:read("*n"), f:read("*n")
    f:read("*n") -- maxval
    f:read(1) -- single whitespace after maxval
    local data = f:read("*a")
    f:close()
    return w, h, data
end

local function capture()
    local path = os.tmpname() .. ".ppm"
    os.remove(path)
    awful.spawn.with_shell("grim -t ppm " .. path)
    async.wait_for_condition(function()
        local f = io.open(path, "r")
        if not f then return false end
        local size = f:seek("end")
        f:close()
        return size > 2000
    end, 10, 0.1)
    async.sleep(0.1)
    local w, h, pix = load_ppm(path)
    os.remove(path)
    return w, h, pix
end

local function rgb_at(pix, w, x, y)
    local i = (y * w + x) * 3 + 1
    return pix:byte(i), pix:byte(i + 1), pix:byte(i + 2)
end

local function is_gray(r, g, b)
    return r >= 120 and r <= 136 and g >= 120 and g <= 136 and b >= 120 and b <= 136
end

local function surface_visible()
    local w, _, pix = capture()
    local r, g, b = rgb_at(pix, w, X, Y)
    return is_gray(r, g, b)
end

runner.run_async(function()
    local bin = find_layer_client()
    assert(bin, "test-layer-client not found")

    local s = screen.primary
    -- Reset to a known layout: B (index 1) selected, A (index 2) claims NS, so
    -- a switch to A slides it in from the right.
    for i = #s.tags, 1, -1 do s.tags[i]:delete() end
    local b = awful.tag.add("B", { screen = s, layout = awful.layout.suit.floating })
    local a = awful.tag.add("A", { screen = s, layout = awful.layout.suit.floating, layers = { NS } })
    b:view_only()

    -- property contract: getter returns the list, setter replaces it + signal.
    assert(type(a.layers) == "table" and a.layers[1] == NS, "tag.layers getter")
    local fired = false
    a:connect_signal("property::layers", function() fired = true end)
    a.layers = { "other" }
    assert(fired, "property::layers signal must fire on set")
    assert(type(a.layers) == "table" and a.layers[1] == "other", "tag.layers setter replaces")
    a.layers = { NS } -- back to the real namespace

    local pid = awful.spawn(string.format(
        "%s --namespace %s --anchor top,left --margin-left 0", bin, NS))
    assert(type(pid) == "number" and pid > 0, "failed to spawn layer surface")

    -- Hidden while its tag (A) is not selected.
    async.sleep(0.5)
    assert(not surface_visible(), "surface must be hidden while its tag is deselected")

    -- Appears when A is selected (wait for the slide-in to settle at anchor).
    slide.set_duration(LONG)
    a:view_only()
    async.wait_for_condition(function()
        return slide.active() or surface_visible()
    end, 10, 0.1)
    async.wait_for_condition(surface_visible, 40, 0.3)
    assert(surface_visible(), "surface must appear with its tag")

    -- Disappears when switching away to B.
    b:view_only()
    async.wait_for_condition(function()
        return not surface_visible()
    end, 40, 0.3)
    assert(not surface_visible(), "surface must disappear when its tag is deselected")

    -- Slides into A: with a long duration, a mid-slide frame shows the surface
    -- off its anchor (still to the right, so the anchor sample reads the
    -- background) and the slide driver is actively running.
    a:view_only()
    async.wait_for_condition(function() return slide.active() end, 10, 0.1)
    async.sleep(0.15)
    assert(slide.active(), "a tag switch with tag.layers must run a slide")
    assert(not surface_visible(), "surface must be off-anchor mid-slide (it slides, not snaps)")
    async.wait_for_condition(surface_visible, 40, 0.3)
    assert(surface_visible(), "surface must settle at its anchor after the slide")

    io.stderr:write("[tag-layers] PASS: surface appears/disappears/slides with its tag\n")
    os.execute("kill " .. pid .. " 2>/dev/null")
    runner.done()
end, { kill_clients = true })