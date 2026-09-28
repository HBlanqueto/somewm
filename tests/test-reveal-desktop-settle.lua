-- Test: the bar must return to its anchor when a transition settles on an
-- ordinary desktop, even though the reveal owner swaps the namespace list to
-- {} (empty) the instant the switch settles - long before a long slide ends.
--
-- On base code the park-release driver gates on the CURRENT namespace list:
-- once the list is empty it stops easing and the flush matches nothing, so the
-- bar stays parked (reveal_offset_y = -range) and is MISSING on the desktop
-- after using a focus/player space. The fix rides the surfaces captured at the
-- deferral (list-independent) and forces any leftover offset back to its
-- anchor.
--
-- Verification is deterministic: the slide is driven with set_progress().
--   parked   reveal.activate(screen, 0)   -> bar at anchor_y - range
--   half     set_progress(0.5) after the switch -> bottom edge at range/2
--            (a 100 px bar parked at -32 has its bottom at 68; at eased 0.5 it
--            has descended to 84, so y=80 is full, y=95 empty)
--   done     set_progress(1.0)            -> bar at its anchor (y=95 full)
-- The "half" and "done" assertions are the regression: a stranded (base) bar
-- leaves y=80 empty at 0.5 and y=95 empty at 1.0.

local runner = require("_runner")
local async = require("_async")
local awful = require("awful")
local gears = require("gears")

if not os.execute("command -v grim >/dev/null 2>&1") then
    io.stderr:write("SKIP: grim not available\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local reveal = _G.reveal
local slide = _G.slide
if not (reveal and reveal.set_layers and reveal.activate and reveal.reset
        and slide and slide.set_duration and slide.set_progress and slide.active) then
    io.stderr:write("SKIP: compositor reveal/slide drivers missing\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local RANGE = 32
local NS = "bar"
local X = 50   -- inside the 100 px bar
local Y_HALF = 80   -- inside at eased 0.5 (bottom edge ~84), not yet at anchor
local Y_EDGE = 95   -- inside only once the release is complete

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

local function load_ppm(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local magic = f:read(2)
    assert(magic == "P6", "not a P6 ppm: " .. magic)
    local w, h = f:read("*n"), f:read("*n")
    f:read("*n")
    f:read(1)
    local data = f:read("*a")
    f:close()
    return w, h, data
end

local function capture()
    for _ = 1, 6 do
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
        async.sleep(0.15)
        local w, h, pix = load_ppm(path)
        os.remove(path)
        if w and h and pix then return w, h, pix end
        async.sleep(0.3)
    end
    return nil
end

local function rgb_at(pix, w, x, y)
    local i = (y * w + x) * 3 + 1
    return pix:byte(i), pix:byte(i + 1), pix:byte(i + 2)
end

-- 50% alpha gray (0x80) over the black fallback backdrop reads as a mid gray.
local function is_gray(r, g, b)
    return r >= 70 and r <= 160 and g >= 70 and g <= 160 and b >= 70 and b <= 160
end

local function sample(y)
    local w, _, pix = capture()
    if not w then return nil end
    local r, g, b = rgb_at(pix, w, X, y)
    io.stderr:write(string.format(
        "[reveal-desktop-settle] y=%d -> %d,%d,%d\n", y, r, g, b))
    return is_gray(r, g, b)
end

runner.run_async(function()
    local bin = find_layer_client()
    assert(bin, "test-layer-client not found")

    reveal.set_layers({ NS }, RANGE)

    -- Mimic the config: leaving a parked space defers the release, and the
    -- reveal owner re-derives the namespace list right after the switch
    -- settles (an ordinary desktop -> {}), which on a long slide happens
    -- long before the slide ends.
    tag.connect_signal("property::selected", function(t)
        if not t.selected then
            reveal.reset()
        end
        gears.timer.delayed_call(function()
            local s = screen.primary
            local sel = s.selected_tag
            reveal.set_layers(sel and sel.role == "player" and { NS } or {}, RANGE)
        end)
    end)

    local s = screen.primary
    for i = #s.tags, 1, -1 do s.tags[i]:delete() end
    local d1 = awful.tag.add("d1", { screen = s })
    local d2 = awful.tag.add("d2", { screen = s })
    d1:view_only()

    -- Drive the setup slide (primes the slide driver) to completion.
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    slide.set_progress(1.0)
    async.sleep(0.2)
    async.wait_for_condition(function() return not slide.active() end, 10, 0.2)

    local pid = awful.spawn(string.format(
        "%s --namespace %s --layer background --anchor top,left", bin, NS))
    assert(type(pid) == "number" and pid > 0, "failed to spawn layer surface")

    -- Baseline: the bar is at its anchor (y=95 full).
    async.wait_for_condition(function() return sample(Y_EDGE) == true end, 30, 0.3)
    assert(sample(Y_EDGE) == true, "bar never mapped at anchor")

    -- Parked (mimic a focus/player space holding the reveal): the reveal owner
    -- would have set the list to { NS } for that space, then parked it.
    reveal.set_layers({ NS }, RANGE)
    reveal.activate(screen.primary, 0)
    async.sleep(0.2)
    assert(sample(Y_EDGE) == false, "parked: bar must leave y=95")

    -- Slow slide to the ordinary desktop.
    pcall(slide.set_duration, 2.0)

    -- Switch to the desktop; the release defers and the list goes empty
    -- mid-slide. At eased 0.5 the bar must be HALF released (bottom edge at
    -- range/2): y=80 full, y=95 empty. A stranded (base) bar leaves both
    -- empty.
    d2:view_only()
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    slide.set_progress(0.5)
    async.sleep(0.2)
    assert(sample(Y_HALF) == true,
        "half: bar must keep descending despite the empty reveal list")
    assert(sample(Y_EDGE) == false,
        "half: bar must not be at its anchor yet at eased 0.5")

    -- Complete the slide: fully released, at its anchor on the desktop.
    slide.set_progress(1.0)
    async.sleep(0.2)
    assert(sample(Y_EDGE) == true,
        "done: bar must return to its anchor when the desktop settles")

    io.stderr:write("[reveal-desktop-settle] PASS: bar returns to its anchor on a desktop settle\n")
    os.execute("kill " .. pid .. " 2>/dev/null")
    runner.done()
end, { kill_clients = true })