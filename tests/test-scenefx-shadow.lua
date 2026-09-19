---------------------------------------------------------------------------
-- Test: SceneFX GPU shadows behind the client shadow property.
--
-- Runs only on a SceneFX build (awesome.scenefx); the disabled build skips.
-- Reads back the applied wlr_scene_shadow parameters through
-- awesome._scenefx_info(c).shadow, which mirrors the node's size, position,
-- corner radius, blur sigma and color.
--
-- Run: SOMEWM=./build-fx/somewm SOMEWM_CLIENT=./build-fx/somewm-client \
--        HEADLESS=0 ./tests/run-integration.sh tests/test-scenefx-shadow.lua
---------------------------------------------------------------------------

local runner      = require("_runner")
local async       = require("_async")
local test_client = require("_client")

if not awesome.scenefx then
    io.stderr:write("SKIP: not a SceneFX build\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

if not test_client.is_available() then
    io.stderr:write("SKIP: no terminal available\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local function cleanup(c)
    if c and c.valid then c:kill() end
    for _, pid in ipairs(test_client.get_spawned_pids()) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    return async.wait_for_no_clients(5)
end

local function near(a, b, eps)
    eps = eps or 0.01
    return math.abs(a - b) <= eps
end

runner.run_async(function()

    ---------------------------------------------------------------------------
    -- TEST 1: shadow config maps onto the SceneFX shadow node
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 1] shadow creation and parameter mapping\n")

    test_client("scenefx_shadow_1")
    local c = async.wait_for_client("scenefx_shadow_1", 5)
    assert(c, "client did not appear")

    c.border_width = 4
    c.corner_radius = 12
    c.shadow = {
        enabled       = true,
        radius        = 10,
        offset_x      = 3,
        offset_y      = 4,
        spread        = 4,
        color         = { 1, 0, 0, 0.5 },
        opacity       = 0.5,
        follow_corners = true,
    }
    async.sleep(0.2)

    local s = awesome._scenefx_info(c).shadow
    assert(s and s.visible == 1, "expected an enabled SceneFX shadow")

    local g = c:geometry()
    local frame_w = g.width + 2 * c.border_width
    local frame_h = g.height + 2 * c.border_width
    assert(s.width == frame_w + 2 * 4 + 2 * 10,
        string.format("expected width %d, got %d", frame_w + 28, s.width))
    assert(s.height == frame_h + 2 * 4 + 2 * 10,
        string.format("expected height %d, got %d", frame_h + 28, s.height))
    assert(s.x == 3 - 4 - 10 and s.y == 4 - 4 - 10,
        string.format("expected pos (%d,%d), got (%d,%d)", -11, -10, s.x, s.y))
    assert(s.corner_radius == 12,
        string.format("expected corner_radius 12, got %d", s.corner_radius))
    assert(near(s.blur_sigma, 10),
        string.format("expected blur_sigma 10, got %s", tostring(s.blur_sigma)))
    assert(near(s.r, 1) and near(s.g, 0) and near(s.b, 0) and near(s.a, 0.25),
        string.format("expected color 1/0/0/0.25, got %s/%s/%s/%s",
            tostring(s.r), tostring(s.g), tostring(s.b), tostring(s.a)))

    io.stderr:write("[TEST 1] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 2: config change recreates the shadow with new parameters
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 2] shadow reconfiguration\n")

    c.shadow = { enabled = true, radius = 5, offset_x = 0, offset_y = 0,
                 color = { 1, 1, 0, 0.5 }, opacity = 0.5 }
    async.sleep(0.2)

    s = awesome._scenefx_info(c).shadow
    assert(s and s.visible == 1, "shadow lost after reconfiguration")
    assert(s.width == frame_w + 2 * 5,
        string.format("expected width %d, got %d", frame_w + 10, s.width))
    assert(s.blur_sigma == 5,
        string.format("expected blur_sigma 5, got %s", tostring(s.blur_sigma)))
    assert(s.x == 0 - 5 and s.y == 0 - 5,
        string.format("expected pos (%d,%d), got (%d,%d)", -5, -5, s.x, s.y))
    assert(near(s.r, 1) and near(s.g, 1) and near(s.b, 0) and near(s.a, 0.25),
        string.format("expected color 1/1/0/0.25, got %s/%s/%s/%s",
            tostring(s.r), tostring(s.g), tostring(s.b), tostring(s.a)))

    io.stderr:write("[TEST 2] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 3: disabling the shadow removes the node
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 3] shadow disable\n")

    c.shadow = false
    async.sleep(0.2)

    s = awesome._scenefx_info(c).shadow
    assert(s and s.visible == nil, "expected no shadow node after disabling")

    io.stderr:write("[TEST 3] PASS\n")

    io.stderr:write("[PASS] all scenefx shadow tests passed\n")
    assert(cleanup(c), "cleanup: client did not close")
    runner.done()
end)