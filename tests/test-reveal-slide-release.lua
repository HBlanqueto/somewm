-- Test: the reveal park releases across a slide, not at the switch.
--
-- Leaving a space that holds the reveal (bar/notch parked up by `range`)
-- used to snap the surfaces back to their anchor the instant the switch ran,
-- so on a focus->desktop transition the bar/notch popped into view over the
-- outgoing desktop while the incoming one was still sliding in. Now the
-- release is driven by the slide driver: the surfaces ease from the parked
-- offset to 0 with the same eased progress as the desktops.
--
-- Verification is deterministic: the slide is driven with set_progress().
--   parked   reveal.activate(screen, 0)   -> all surfaces at anchor_y - range
--   half     set_progress(0.5) after the switch -> bottom edge at range*0.5
--            (a 100 px surface parked at -32 has its bottom at 68; at eased
--             0.5 it has descended to 84, so y=90 is still empty, y=80 full)
--   done     set_progress(1.0)            -> all at anchor (y=90 full again)
-- The "half" assertion is the regression: a snap-to-anchor would fill y=90
-- the instant the switch ran.

local runner = require("_runner")
local async = require("_async")
local awful = require("awful")

if not os.execute("command -v grim >/dev/null 2>&1") then
    io.stderr:write("SKIP: grim not available\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local reveal = _G.reveal
if not (reveal and reveal.set_layers and reveal.activate and reveal.reset) then
    io.stderr:write("SKIP: compositor reveal driver missing\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local RANGE = 32
local NS = { "neptune-bar", "notch", "reveal-test" }
local XS = { 0, 110, 220 }
local Y_PARKED = 90   -- inside the surface when revealed, below its parked bottom
local Y_HALF = 80     -- inside at eased 0.5 (bottom at 84), not yet at eased 0
local Y_EDGE = 95     -- inside only once the release is complete (bottom 100)

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

-- 50% alpha gray over a black background reads as a mid gray (not black).
local function is_gray(r, g, b)
    return r >= 70 and r <= 160 and g >= 70 and g <= 160 and b >= 70 and b <= 160
end

local function read_row(w, _h, pix, y, label)
    local out = {}
    for i, x in ipairs(XS) do
        local r, g, b = rgb_at(pix, w, x, y)
        out[i] = is_gray(r, g, b)
    end
    io.stderr:write(string.format(
        "[reveal-slide-release] %-7s y=%d samples=%s,%s,%s\n",
        label, y, tostring(out[1]), tostring(out[2]), tostring(out[3])))
    return out
end

runner.run_async(function()
    local bin = find_layer_client()
    assert(bin, "test-layer-client not found")

    reveal.set_layers(NS, RANGE)

    local pids = {}
    for i, ns in ipairs(NS) do
        local pid = awful.spawn(string.format(
            "%s --namespace %s --anchor top,left --margin-left %d",
            bin, ns, XS[i]))
        assert(type(pid) == "number" and pid > 0, "failed to spawn " .. ns)
        pids[#pids + 1] = pid
    end

    -- Surfaces map at their anchors (y=90 full).
    local baseline
    async.wait_for_condition(function()
        local w, h, pix = capture()
        baseline = read_row(w, h, pix, Y_PARKED, "mapping")
        return baseline[1] and baseline[2] and baseline[3]
    end, 20, 0.3)
    assert(baseline[1] and baseline[2] and baseline[3],
        "layer surfaces never mapped at anchor")

    -- Parked: one screen call moves all three up by RANGE (y=90 empty).
    reveal.activate(screen.primary, 0)
    async.sleep(0.1)
    local w, h, pix = capture()
    local parked = read_row(w, h, pix, Y_PARKED, "parked")
    assert(not parked[1] and not parked[2] and not parked[3],
        "parked: every namespace must leave y=90 (moved up by range)")

    -- Ensure a second desktop exists for the switch.
    if #screen.primary.tags < 2 then
        awful.tag.add("two", { screen = screen.primary })
    end

    -- Slow slide, two desktops.
    pcall(slide.set_duration, 2.0)

    -- Establish the previous-selection snapshot: the slide only takes a 1->1
    -- switch once a previous tag is known, so prime it with a harmless round
    -- trip (each slide finished immediately) before the reveal is parked.
    local t1 = screen.primary.tags[1]
    local t2 = screen.primary.tags[2]
    t2:view_only()
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    pcall(slide.set_progress, 1.0)
    async.sleep(0.1)
    t1:view_only()
    async.wait_for_condition(function() return slide.active() == true end, 5, 0.1)
    pcall(slide.set_progress, 1.0)
    async.sleep(0.1)

    -- The switch defers the park release: reset runs inside the switch, and a
    -- slide takes over the release (this test also asserts the deferral by
    -- checking the release is still partial mid-slide).
    tag.connect_signal("property::selected", function(t)
        if t and t.selected then reveal.reset() end
    end)

    -- Switch to tag 2 (the slide starts in the banning pass).
    t2:view_only()

    -- Let the slide start, then drive it to 50 %: the surfaces must be HALF
    -- released (bottom edge at range/2), so y=80 full and y=95 empty. An
    -- instant snap would already fill y=95 here.
    async.wait_for_condition(function()
        return slide.active() == true
    end, 5, 0.1)
    slide.set_progress(0.5)
    async.sleep(0.2)
    w, h, pix = capture()
    local half_80 = read_row(w, h, pix, Y_HALF, "half-80")
    local half_95 = read_row(w, h, pix, Y_EDGE, "half-95")
    assert(half_80[1] and half_80[2] and half_80[3],
        "half: every namespace must cover y=80 at eased 0.5 (descending)")
    assert(not half_95[1] and not half_95[2] and not half_95[3],
        "half: no namespace may cover y=95 at eased 0.5 (release is gradual, "
        .. "not an instant snap)")

    -- Complete the slide: fully released, y=95 full again.
    slide.set_progress(1.0)
    async.sleep(0.2)
    w, h, pix = capture()
    local done_95 = read_row(w, h, pix, Y_EDGE, "done")
    assert(done_95[1] and done_95[2] and done_95[3],
        "done: every namespace must cover y=95 once the release completes")

    io.stderr:write("[reveal-slide-release] PASS: park release rides the slide\n")
    for _, pid in ipairs(pids) do
        os.execute("kill " .. pid .. " 2>/dev/null")
    end
    runner.done()
end, { kill_clients = true })