---------------------------------------------------------------------------
-- Test: SceneFX backdrop blur behind client and layer-surface properties.
--
-- Runs only on a SceneFX build (awesome.scenefx); the disabled build skips.
-- Reads the applied wlr_scene_blur node parameters through
-- awesome._scenefx_info(obj).blur, which mirrors the node's size, position,
-- corner radii, alpha, strength and transparency-mask presence.
--
-- Run: SOMEWM=./build-fx/somewm SOMEWM_CLIENT=./build-fx/somewm-client \
--        HEADLESS=0 ./tests/run-integration.sh tests/test-scenefx-blur.lua
---------------------------------------------------------------------------

local runner      = require("_runner")
local async       = require("_async")
local test_client = require("_client")
local awful       = require("awful")

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

local spawn_pids = {}

local function cleanup()
    for _, pid in ipairs(spawn_pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    spawn_pids = {}
    for _, pid in ipairs(test_client.get_spawned_pids()) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    return async.wait_for_no_clients(5)
end

runner.run_async(function()

    ---------------------------------------------------------------------------
    -- TEST 1: client backdrop_blur creates a masked node under the content
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 1] client blur node creation\n")

    test_client("scenefx_blur_1")
    local c = async.wait_for_client("scenefx_blur_1", 5)
    assert(c, "client did not appear")

    c.border_width = 0
    c.corner_radius = 12
    c.backdrop_blur = true
    async.sleep(0.2)

    local blur = awesome._scenefx_info(c).blur
    assert(blur and blur.visible == 1,
        "expected a blur node after backdrop_blur = true")
    assert(blur.width > 0 and blur.height > 0,
        string.format("expected a sized blur box, got %dx%d",
            blur.width or 0, blur.height or 0))
    assert(blur.has_mask == true, "expected a transparency mask source")
    assert(blur.alpha == 1 and blur.strength == 1,
        string.format("expected default alpha/strength 1/1, got %s/%s",
            tostring(blur.alpha), tostring(blur.strength)))
    local content = awesome._scenefx_info(c).content
    assert(content, "expected content corner readback")
    assert(blur.corners and blur.corners.tl == content.tl
            and blur.corners.tr == content.tr and blur.corners.br == content.br
            and blur.corners.bl == content.bl,
        string.format("blur corners (%s,%s,%s,%s) must follow content corners (%s,%s,%s,%s)",
            blur.corners and tostring(blur.corners.tl) or "nil",
            blur.corners and tostring(blur.corners.tr) or "nil",
            blur.corners and tostring(blur.corners.br) or "nil",
            blur.corners and tostring(blur.corners.bl) or "nil",
            content.tl, content.tr, content.br, content.bl))

    io.stderr:write("[TEST 1] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 2: config table (radius, alpha, strength) maps onto the node
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 2] blur config table\n")

    local function nearly(a, b)
    return math.abs((a or -1) - b) < 0.01
end

    c.backdrop_blur = { corner_radius = 8, alpha = 0.5, strength = 0.6 }
    async.sleep(0.2)

    blur = awesome._scenefx_info(c).blur
    assert(blur and blur.visible == 1, "blur node lost on reconfiguration")
    assert(blur.corners and blur.corners.tl == 8,
        string.format("expected config corner_radius 8, got %s",
            blur.corners and tostring(blur.corners.tl) or "nil"))
    assert(nearly(blur.alpha, 0.5) and nearly(blur.strength, 0.6),
        string.format("expected alpha/strength 0.5/0.6, got %s/%s",
            tostring(blur.alpha), tostring(blur.strength)))

    io.stderr:write("[TEST 2] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 3: fullscreen removes the blur, un-fullscreen restores it
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 3] blur and fullscreen\n")

    c.fullscreen = true
    async.sleep(0.2)

    blur = awesome._scenefx_info(c).blur
    assert(blur and not blur.visible,
        "expected no blur node while fullscreen")

    c.fullscreen = false
    async.sleep(0.2)

    blur = awesome._scenefx_info(c).blur
    assert(blur and blur.visible == 1, "blur node not restored after fullscreen")
    assert(blur.corners and blur.corners.tl == 8,
        "expected restored blur to keep its corner radii")

    io.stderr:write("[TEST 3] PASS\n")

    ---------------------------------------------------------------------------
    -- TEST 4: disabling removes the node
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 4] blur disable\n")

    c.backdrop_blur = false
    async.sleep(0.2)

    blur = awesome._scenefx_info(c).blur
    assert(blur and not blur.visible, "blur node stayed after disabling")

    io.stderr:write("[TEST 4] PASS\n")

    -- Re-enable so the destroy-path cleanup also sees a live node.
    c.backdrop_blur = true
    async.sleep(0.2)
    assert(awesome._scenefx_info(c).blur.visible == 1,
        "failed to re-enable blur for cleanup")
    assert(cleanup(), "cleanup: client did not close")

    ---------------------------------------------------------------------------
    -- TEST 5: layer-surface backdrop_blur
    ---------------------------------------------------------------------------
    if not layer_client_ok then
        io.stderr:write("SKIP: test-layer-client not built\n")
    else
        io.stderr:write("[TEST 5] layer surface blur node\n")

        spawn_pids[#spawn_pids + 1] = awful.spawn(
            TEST_LAYER_CLIENT .. " --namespace test-blur")
        local ls
        for _ = 1, 50 do
            async.sleep(0.1)
            for _, s in ipairs(layer_surface.get()) do
                if s.namespace and s.namespace:match("^test%-blur$") then
                    ls = s
                    break
                end
            end
            if ls then break end
        end
        assert(ls, "layer surface did not appear")

        ls.corner_radius = 6
        ls.backdrop_blur = { corner_radius = 6, alpha = 0.8 }
        async.sleep(0.2)

        blur = awesome._scenefx_info(ls).blur
        assert(blur and blur.visible == 1,
            "expected a layer-surface blur node")
        assert(blur.corners and blur.corners.tl == 6,
            string.format("expected layer blur corners 6, got %s",
                blur.corners and tostring(blur.corners.tl) or "nil"))
        assert(nearly(blur.alpha, 0.8),
            string.format("expected layer blur alpha 0.8, got %s",
                tostring(blur.alpha)))
        assert(blur.has_mask == true, "expected a mask on the layer blur")

        ls.backdrop_blur = false
        async.sleep(0.2)

        blur = awesome._scenefx_info(ls).blur
        assert(blur and not blur.visible,
            "layer-surface blur node stayed after disabling")

        io.stderr:write("[TEST 5] PASS\n")
    end

    ---------------------------------------------------------------------------
    -- TEST 6: awesome.set_blur_data is accepted without error
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 6] global blur data\n")

    awesome.set_blur_data(4, 8, 0.05, 1.0, 1.0, 1.0)
    awesome.set_blur_data(2, 5, 0.1, 1.0, 1.0, 1.0)

    io.stderr:write("[TEST 6] PASS\n")

    io.stderr:write("[PASS] all scenefx blur tests passed\n")
    assert(cleanup(), "cleanup: layer client did not close")
    runner.done()
end)