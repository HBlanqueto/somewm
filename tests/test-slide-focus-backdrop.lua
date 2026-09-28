-- Test: sliding into a black-backdrop (focus) space must not black out the
-- outgoing desktop for the whole slide.
--
-- The persistent focus-space black rect (m->black_bg) used to be enabled at
-- the transition start (when the arriving tag is black) and, being a full-
-- screen rect raised to the top of the background layer, covered the outgoing
-- desktop AND the player surface for the entire player->focus slide. The
-- reverse direction (focus->player) was never affected, because there the
-- arriving tag is not black so the rect was disabled first. This test pins the
-- asymmetry: at eased 0.5 of the player->focus slide the player must be DRAWN
-- (its 50% gray pixel), not black; the focus->player midpoint must also draw
-- it.
--
-- A single 100x100 gray layer surface (namespace "player", BACKGROUND layer so
-- the persistent black rect can cover it) is anchored top,left at the RIGHT
-- edge of the workarea, so at eased 0.5 of the outgoing slide (direction +1)
-- it has slid to a still on-screen column. The sample column is computed from
-- the slide geometry (width + gap), not hardcoded.

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
if not (slide and slide.set_duration and slide.set_progress and slide.active) then
    io.stderr:write("SKIP: compositor slide driver missing\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local NS = "player"
local SIZE = 100
local Y = 50 -- inside the top-anchored surface

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
    -- grim can transiently fail ("failed to screenshoot all sources") while
    -- the nested compositor is repainting a slide; retry with a settle delay.
    -- Failure is not silent: the caller asserts on a nil return.
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

local function is_black(r, g, b)
    return r < 25 and g < 25 and b < 25
end

runner.run_async(function()
    local bin = find_layer_client()
    assert(bin, "test-layer-client not found")

    local s = screen.primary
    local wa = s.workarea
    local margin_left = wa.width - SIZE

    -- Reset to a known layout: player (index 1, claims the "player" namespace)
    -- left of focus (index 2, black backdrop), so player->focus slides with
    -- direction +1 (the outgoing desktop, and the player with it, slides left).
    for i = #s.tags, 1, -1 do s.tags[i]:delete() end
    local player = awful.tag.add("player", {
        screen = s, layout = awful.layout.suit.floating, layers = { NS },
    })
    local focus = awful.tag.add("focus", {
        screen = s, layout = awful.layout.suit.floating, backdrop = "black",
    })
    assert(player.index < focus.index, "player must be left of focus (direction +1)")
    player:view_only()

    -- The tag setup can start a slide of its own (it primes the slide
    -- driver's previous-selection snapshot). Drive it to completion so the
    -- steps below start from a steady state, never a running clock-driven
    -- slide.
    async.wait_for_condition(function() return slide.active() == true end, 10, 0.1)
    slide.set_progress(1.0)
    async.sleep(0.2)
    async.wait_for_condition(function()
        return not slide.active() and player.selected == true
    end, 20, 0.2)

    local pid = awful.spawn(string.format(
        "%s --namespace %s --layer background --anchor top,left --margin-left %d",
        bin, NS, margin_left))
    assert(type(pid) == "number" and pid > 0, "failed to spawn layer surface")

    slide.set_duration(2.0)

    -- Sample column at eased 0.5: the outgoing (player) desktop has slid left
    -- by half the monitor width plus gap; the surface, anchored at the right
    -- edge, still covers this column. Same for the incoming side (same offset
    -- magnitude, opposite sign).
    local function half_x()
        local gap = slide.gap and slide.gap() or 80
        local half = math.floor(0.5 * (wa.width + gap))
        return margin_left + SIZE / 2 - half
    end
    local sx = wa.x + half_x()

    -- Baseline: the player is visible at its anchor while its tag is selected.
    async.wait_for_condition(function()
        local w, _, pix = capture()
        if not w then return false end
        local r, g, b = rgb_at(pix, w, wa.x + margin_left + SIZE / 2, Y)
        return is_gray(r, g, b)
    end, 20, 0.3)
    io.stderr:write("[slide-focus-backdrop] baseline: player visible at anchor\n")

    -- Player -> focus: the reported blackout direction. At the midpoint the
    -- player must be DRAWN (gray), not black.
    focus:view_only()
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    slide.set_progress(0.5)
    async.sleep(0.2)
    local w1, _, pix1 = capture()
    assert(w1, "player->focus midpoint capture failed")
    local r1, g1, b1 = rgb_at(pix1, w1, sx, Y)
    io.stderr:write(string.format(
        "[slide-focus-backdrop] player->focus half: r=%d g=%d b=%d\n", r1, g1, b1))
    assert(is_gray(r1, g1, b1),
        "player->focus: player must be DRAWN (not black) at the midpoint")

    -- Complete the slide: the focus space settles black (persistent backdrop),
    -- the player is hidden with its deselected tag.
    slide.set_progress(1.0)
    async.sleep(0.2)
    local w2, _, pix2 = capture()
    assert(w2, "player->focus settle capture failed")
    local r2, g2, b2 = rgb_at(pix2, w2, sx, Y)
    io.stderr:write(string.format(
        "[slide-focus-backdrop] focus settled: r=%d g=%d b=%d\n", r2, g2, b2))
    assert(is_black(r2, g2, b2), "focus space must settle black")

    -- Focus -> player: the reverse direction draws the player at the midpoint
    -- too (it never blacked out; the blackout was one-directional).
    player:view_only()
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    slide.set_progress(0.5)
    async.sleep(0.2)
    local w3, _, pix3 = capture()
    assert(w3, "focus->player midpoint capture failed")
    local r3, g3, b3 = rgb_at(pix3, w3, sx, Y)
    io.stderr:write(string.format(
        "[slide-focus-backdrop] focus->player half: r=%d g=%d b=%d\n", r3, g3, b3))
    assert(is_gray(r3, g3, b3),
        "focus->player: player must be drawn at the midpoint")

    -- Settle back on the player space: the player is at its anchor again.
    slide.set_progress(1.0)
    async.sleep(0.2)
    local w4, _, pix4 = capture()
    assert(w4, "focus->player settle capture failed")
    local r4, g4, b4 = rgb_at(pix4, w4, wa.x + margin_left + SIZE / 2, Y)
    io.stderr:write(string.format(
        "[slide-focus-backdrop] player settled: r=%d g=%d b=%d\n", r4, g4, b4))
    assert(is_gray(r4, g4, b4), "player must return to its anchor after the slide")

    io.stderr:write("[slide-focus-backdrop] PASS: outgoing desktop (and player) keep "
        .. "drawing during a slide into a black space\n")
    os.execute("kill " .. pid .. " 2>/dev/null")
    runner.done()
end, { kill_clients = true })