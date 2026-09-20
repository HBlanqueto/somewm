---------------------------------------------------------------------------
-- Test: opacity-correct fades for windows and layer-shell panels.
--
-- Drives `somewm.fade` (on top of `awesome.start_animation`) and checks
-- the fade always ends at the object's target opacity -- never at a
-- hardcoded 1.0 -- with decorations following: border and shadow color
-- alpha, blur alpha/strength, and every buffer in the surface tree.
--
-- On a SceneFX build the scene state is read back through
-- awesome._scenefx_info(); the disabled build asserts the Lua-level end
-- values only (buffers fade there, decorations stay -- see DEVIATIONS).
--
-- Run: SOMEWM=./build-fx/somewm SOMEWM_CLIENT=./build-fx/somewm-client \
--        HEADLESS=0 ./tests/run-integration.sh tests/test-scenefx-fade.lua
---------------------------------------------------------------------------

local runner      = require("_runner")
local async       = require("_async")
local test_client = require("_client")
local awful       = require("awful")
local fade        = require("somewm.fade")

local scenefx = awesome.scenefx

if not test_client.is_available() then
    io.stderr:write("SKIP: no terminal available\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local function find_binary(name)
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    for _, candidate in ipairs({
        build_dir .. "/" .. name,
        "./build-test/" .. name,
        "./build/" .. name,
    }) do
        local f = io.open(candidate, "r")
        if f then f:close() return candidate end
    end
    return nil
end

local TEST_LAYER_CLIENT = find_binary("test-layer-client")
local layer_client_ok = TEST_LAYER_CLIENT ~= nil

local function nearly(a, b)
    return math.abs((a or -1) - b) < 0.02
end

local function cleanup()
    os.execute("pkill -9 test-layer-client 2>/dev/null")
    for _, pid in ipairs(test_client.get_spawned_pids()) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    return async.wait_for_no_clients(5)
end

runner.run_async(function()

    ---------------------------------------------------------------------------
    -- TEST 1: fade.in ends at the rule-set target, decorations follow
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 1] fade.in ends at target opacity\n")

    test_client("scenefx_fade_1")
    local c = async.wait_for_client("scenefx_fade_1", 5)
    assert(c, "client did not appear")

    c.border_width = 2
    c.corner_radius = 12
    c.shadow = { enabled = true }
    c.backdrop_blur = true
    async.sleep(0.3)

    local base
    if scenefx then
        base = awesome._scenefx_info(c)
        assert(base and base.content, "expected content readback")
        assert(base.border_alpha ~= nil, "expected a border frame node")
        assert(base.shadow and base.shadow.a ~= nil,
            "expected a shadow node")
        assert(base.blur and base.blur.visible == 1,
            "expected a blur node")
    end

    -- A rule sets the target opacity; the fade must end there, not at 1.0.
    c.opacity = 0.6
    fade.fade_in(c, { duration = 0.3 })
    assert(async.wait_for_condition(function()
        return nearly(c.opacity, 0.6)
    end, 5), string.format("fade did not end at 0.6 (got %s)",
        tostring(c.opacity)))

    if scenefx then
        local info = awesome._scenefx_info(c)
        assert(nearly(info.content.opacity, 0.6),
            string.format("content opacity %s, want 0.6",
                tostring(info.content.opacity)))
        assert(nearly(info.border_alpha, base.border_alpha * 0.6),
            string.format("border alpha %s, want %s",
                tostring(info.border_alpha),
                tostring(base.border_alpha * 0.6)))
        assert(nearly(info.shadow.a, base.shadow.a * 0.6),
            string.format("shadow alpha %s, want %s",
                tostring(info.shadow.a),
                tostring(base.shadow.a * 0.6)))
        assert(nearly(info.blur.alpha, 0.6)
                and nearly(info.blur.strength, 0.6),
            string.format("blur alpha/strength %s/%s, want 0.6/0.6",
                tostring(info.blur.alpha), tostring(info.blur.strength)))
    end

    io.stderr:write("[TEST 1] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 2: fade.out ends at zero, fade.in restores the target
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 2] fade.out to zero, fade.in restores\n")

    local out_done = false
    fade.fade_out(c, { duration = 0.2, done = function() out_done = true end })
    assert(async.wait_for_condition(function()
        return out_done and nearly(c.opacity, 0)
    end, 5), string.format("fade.out did not end at 0 (got %s)",
        tostring(c.opacity)))

    if scenefx then
        local info = awesome._scenefx_info(c)
        assert(nearly(info.border_alpha, 0),
            "border did not fade out with the window")
        assert(nearly(info.shadow.a, 0),
            "shadow did not fade out with the window")
    end

    fade.fade_in(c, { duration = 0.2 })
    assert(async.wait_for_condition(function()
        return nearly(c.opacity, 0.6)
    end, 5), string.format("fade.in did not restore 0.6 (got %s)",
        tostring(c.opacity)))

    io.stderr:write("[TEST 2] PASS\n")
    assert(cleanup(), "cleanup: client did not close")

    ---------------------------------------------------------------------------
    -- TEST 3: layer_surface.opacity fades the whole panel tree
    ---------------------------------------------------------------------------
    if not layer_client_ok then
        io.stderr:write("SKIP: test-layer-client not built\n")
    else
        io.stderr:write("[TEST 3] layer surface fade\n")

        awful.spawn(TEST_LAYER_CLIENT .. " --namespace test-fade")
        local ls
        assert(async.wait_for_condition(function()
            for _, s in ipairs(layer_surface.get()) do
                if s.namespace and s.namespace:match("^test%-fade$") then
                    ls = s
                    return true
                end
            end
        end, 5), "layer surface did not appear")

        ls.backdrop_blur = true
        async.sleep(0.3)

        ls.opacity = 0.7
        fade.fade_in(ls, { duration = 0.3 })
        assert(async.wait_for_condition(function()
            return nearly(ls.opacity, 0.7)
        end, 5), string.format("panel fade did not end at 0.7 (got %s)",
            tostring(ls.opacity)))

        if scenefx then
            local info = awesome._scenefx_info(ls)
            assert(info and info.content, "expected panel content readback")
            assert(nearly(info.content.opacity, 0.7),
                string.format("panel content opacity %s, want 0.7",
                    tostring(info.content.opacity)))
            assert(info.blur and nearly(info.blur.alpha, 0.7),
                string.format("panel blur alpha %s, want 0.7",
                    tostring(info.blur and info.blur.alpha)))
        end

        io.stderr:write("[TEST 3] PASS\n")
    end

    io.stderr:write("[PASS] all scenefx fade tests passed\n")
    assert(cleanup(), "cleanup: layer client did not close")
    runner.done()
end)
