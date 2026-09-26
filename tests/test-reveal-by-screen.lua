-- Test: reveal.activate() driven from a SCREEN, with zero window clients.
--
-- The compositor's reveal driver used to resolve the target monitor from a
-- client (reveal.activate(client, v)); a space that owns no window (e.g. the
-- player space) had nothing to borrow. reveal.activate() now also accepts a
-- screen and drives the reveal on that screen's monitor directly.
--
-- Three layer surfaces with registered namespaces are spawned (bar, notch and
-- a third test namespace), all anchored top,left at x = 0/110/220 with a gray
-- 100x100 buffer. NO xdg-toplevel client exists anywhere. Every state is read
-- from a grim capture: at y=90 a surface reads gray (128,128,128) when its node
-- sits at the anchor, and reads the background when parked up by `range`.
--   parked   reveal.activate(screen, 0)   -> all three at anchor_y - range
--   revealed reveal.activate(screen, 32)  -> all three at anchor
--   reset    reveal.reset()               -> all three back at anchor
-- "Moved in one tick" means one capture after one call shows all three at the
-- same offset; each assertion checks all three namespaces together.

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
local Y = 90 -- inside the surface when revealed, below its parked bottom edge

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

-- rgb at (x, y) in a P6 payload of width w.
local function rgb_at(pix, w, x, y)
    local i = (y * w + x) * 3 + 1
    return pix:byte(i), pix:byte(i + 1), pix:byte(i + 2)
end

local function is_gray(r, g, b)
    return r >= 120 and r <= 136 and g >= 120 and g <= 136 and b >= 120 and b <= 136
end

-- Every namespace surface must read gray at its sample point.
local function read_surfaces(w, _h, pix, label)
    local out = {}
    for i, x in ipairs(XS) do
        local r, g, b = rgb_at(pix, w, x, Y)
        out[i] = is_gray(r, g, b)
    end
    io.stderr:write(string.format(
        "[reveal-by-screen] %-9s samples=%s,%s,%s\n", label,
        tostring(out[1]), tostring(out[2]), tostring(out[3])))
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

    -- Wait until all three surfaces map at their anchors (gray at y=90).
    local baseline
    async.wait_for_condition(function()
        local w, h, pix = capture()
        baseline = read_surfaces(w, h, pix, "mapping")
        return baseline[1] and baseline[2] and baseline[3]
    end, 20, 0.3)
    assert(baseline[1] and baseline[2] and baseline[3],
        "layer surfaces never mapped at anchor")

    -- Parked: one screen call, all three shift up by RANGE together.
    reveal.activate(screen.primary, 0)
    async.sleep(0.1)
    local w, h, pix = capture()
    local parked = read_surfaces(w, h, pix, "parked")
    assert(not parked[1] and not parked[2] and not parked[3],
        "parked: every namespace must leave the sample point (moved up)")

    -- Revealed: one screen call at the full range, all three back at anchor.
    reveal.activate(screen.primary, RANGE)
    async.sleep(0.1)
    w, h, pix = capture()
    local shown = read_surfaces(w, h, pix, "revealed")
    assert(shown[1] and shown[2] and shown[3],
        "revealed: every namespace must return to the anchor in one tick")

    -- Reset: surfaces return to their anchors, reveal active flag cleared.
    reveal.reset()
    async.sleep(0.1)
    w, h, pix = capture()
    local reset = read_surfaces(w, h, pix, "reset")
    assert(reset[1] and reset[2] and reset[3],
        "reset: every namespace must be back at the anchor")
    assert(reveal.active() == false, "reset must clear the reveal active flag")

    io.stderr:write("[reveal-by-screen] PASS: screen-driven reveal moves all namespaces\n")
    for _, pid in ipairs(pids) do
        os.execute("kill " .. pid .. " 2>/dev/null")
    end
    runner.done()
end, { kill_clients = true })