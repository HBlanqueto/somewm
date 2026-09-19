---------------------------------------------------------------------------
--- Test: SceneFX shader corner radii for clients, titlebars and layer surfaces.
---
--- Runs only on a SceneFX build (awesome.scenefx); the disabled build skips.
--- Reads back the applied radii through awesome._scenefx_info(), which mirrors
--- the fx_corner_radii on each scene buffer.
---
--- Run: SOMEWM=./build-fx/somewm SOMEWM_CLIENT=./build-fx/somewm-client \
---        HEADLESS=0 ./tests/run-integration.sh tests/test-scenefx-corner-radii.lua
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
if not TEST_LAYER_CLIENT then
    io.stderr:write("SKIP: test-layer-client not found\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local function radii(info)
    if not info or not info.content then return nil end
    return info.content.tl, info.content.tr, info.content.br, info.content.bl
end

local function cleanup(c)
    if c and c.valid then c:kill() end
    for _, pid in ipairs(test_client.get_spawned_pids()) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    os.execute("pkill -9 test-layer-client 2>/dev/null")
    return async.wait_for_no_clients(5)
end

runner.run_async(function()

    ---------------------------------------------------------------------------
    -- TEST 1: client content corners follow corner_radius minus border width
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 1] client content corner radii\n")

    test_client("scenefx_cr_1")
    local c = async.wait_for_client("scenefx_cr_1", 5)
    assert(c, "client did not appear")

    c.border_width = 4
    c.corner_radius = 12
    async.sleep(0.2)

    local info = awesome._scenefx_info(c)
    local tl, tr, br, bl = radii(info)
    -- inner radius = frame radius (12) - border width (4) = 8 on every corner
    assert(tl == 8 and tr == 8 and br == 8 and bl == 8,
        string.format("expected content radii 8/8/8/8, got %s/%s/%s/%s",
            tostring(tl), tostring(tr), tostring(br), tostring(bl)))

    io.stderr:write("[TEST 1] PASS content=8/8/8/8\n")

    ---------------------------------------------------------------------------
    -- TEST 2: a top titlebar takes over the top corners
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 2] titlebar corners\n")

    awful.titlebar(c, { size = 29, position = "top" })
    async.sleep(0.2)

    info = awesome._scenefx_info(c)
    assert(info and info.titlebars, "no titlebar radii reported")
    local tt = info.titlebars[1]  -- CLIENT_TITLEBAR_TOP
    assert(tt and tt.tl == 8 and tt.tr == 8 and tt.br == 0 and tt.bl == 0,
        string.format("expected top titlebar radii 8/8/0/0, got %s/%s/%s/%s",
            tostring(tt and tt.tl), tostring(tt and tt.tr),
            tostring(tt and tt.br), tostring(tt and tt.bl)))

    tl, tr, br, bl = radii(info)
    assert(tl == 0 and tr == 0 and br == 8 and bl == 8,
        string.format("content top corners should be square under a titlebar, got %s/%s/%s/%s",
            tostring(tl), tostring(tr), tostring(br), tostring(bl)))

    io.stderr:write("[TEST 2] PASS titlebar=8/8/0/0 content=0/0/8/8\n")

    ---------------------------------------------------------------------------
    -- TEST 3: disabling rounding clears every radius
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 3] corner_radius off clears radii\n")

    c.corner_radius = false
    async.sleep(0.2)

    info = awesome._scenefx_info(c)
    tl, tr, br, bl = radii(info)
    assert(tl == 0 and tr == 0 and br == 0 and bl == 0,
        "expected zero radii after disabling")

    io.stderr:write("[TEST 3] PASS\n")
    assert(cleanup(c), "cleanup: client did not close")

    ---------------------------------------------------------------------------
    -- TEST 4: layer surface corner radius
    ---------------------------------------------------------------------------
    io.stderr:write("[TEST 4] layer surface corner radii\n")

    awful.spawn(TEST_LAYER_CLIENT .. " --namespace scenefx-cr-layer")
    local ls
    async.wait_for_condition(function()
        for _, l in ipairs(layer_surface.get()) do
            if l.namespace and l.namespace:match("scenefx%-cr%-layer") then
                ls = l
                return true
            end
        end
    end, 5)

    assert(ls, "layer surface did not appear")
    ls.corner_radius = 10
    async.sleep(0.2)

    local linfo = awesome._scenefx_info(ls)
    tl, tr, br, bl = radii(linfo)
    assert(tl == 10 and tr == 10 and br == 10 and bl == 10,
        string.format("expected layer radii 10/10/10/10, got %s/%s/%s/%s",
            tostring(tl), tostring(tr), tostring(br), tostring(bl)))

    io.stderr:write("[TEST 4] PASS layer=10/10/10/10\n")

    os.execute("pkill -9 test-layer-client 2>/dev/null")
    async.wait_for_no_clients(5)

    io.stderr:write("[PASS] all scenefx corner radii tests passed\n")
    runner.done()
end)
