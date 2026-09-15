---------------------------------------------------------------------------
--- Visual showcase: rounded-corner shadows following per-corner rounding.
---
--- Spawns five 4-quadrant pattern clients, each with a different corner
--- rounding and a different shadow (crisp/solid vs soft/translucent), then
--- captures the real compositor output with grim (wlr-screencopy) and saves
--- the PNGs under docs/:
---
---   docs/shadow-corners-overview.png            all five together
---   docs/shadow-corners-1-square-solid.png      square + crisp shadow
---   docs/shadow-corners-2-rounded-solid.png     uniform rounded + crisp shadow
---   docs/shadow-corners-3-top-rounded-solid.png top-rounded + crisp shadow
---   docs/shadow-corners-4-bottom-soft.png       bottom-rounded + soft shadow
---   docs/shadow-corners-5-diagonal-soft.png     diagonal-rounded + soft shadow
---
--- Run (needs grim on PATH):
---   nix-shell -p luajit luajitPackages.lgi bc grim --run '
---     cd /home/humbe/somewm && VERBOSE=1 TEST_TIMEOUT=60 HEADLESS=1 \
---       SOMEWM=./build-test/somewm SOMEWM_CLIENT=./build-test/somewm-client \
---       ./tests/run-integration.sh tests/demo-shadow-corners.lua'
---------------------------------------------------------------------------

local awful  = require("awful")
local async  = require("_async")
local runner = require("_runner")

local APP_ID = "content_pattern_test"
local DOCS_DIR = "docs"

local EXAMPLES = {
    { name = "1-square-solid",      radii = { 0,  0,  0,  0 }, shadow = { spread = 10, radius = 5,  opacity = 0.90 } },
    { name = "2-rounded-solid",     radii = { 18, 18, 18, 18 }, shadow = { spread = 10, radius = 5,  opacity = 0.90 } },
    { name = "3-top-rounded-solid", radii = { 26, 26, 0,  0  }, shadow = { spread = 10, radius = 5,  opacity = 0.90 } },
    { name = "4-bottom-soft",       radii = { 0,  0,  26, 26 }, shadow = { spread = 18, radius = 22, opacity = 0.45 } },
    { name = "5-diagonal-soft",     radii = { 30, 0,  30, 0  }, shadow = { spread = 22, radius = 30, opacity = 0.30 } },
}

local function find_binary()
    local somewm = os.getenv("SOMEWM") or "./build-test/somewm"
    local build_dir = somewm:match("^(.*)/somewm$") or "./build-test"
    for _, candidate in ipairs({
        build_dir .. "/test-content-pattern-client",
        "./build/test-content-pattern-client",
        "./build-test/test-content-pattern-client",
    }) do
        local f = io.open(candidate, "r")
        if f then f:close(); return candidate end
    end
end

local BINARY = find_binary()

-- grim must be on PATH; otherwise there is nothing to capture with.
local function grim_available()
    local ok = os.execute("command -v grim >/dev/null 2>&1")
    return ok == 0 or ok == true
end

if not BINARY then
    io.stderr:write("SKIP: test-content-pattern-client not found\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

if not grim_available() then
    io.stderr:write("SKIP: grim not found on PATH (needed for the screencopy capture)\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local clients = {}   -- in spawn order
local pids = {}

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
    -- Let grim finish writing (file size stable).
    async.sleep(0.3)
    io.stderr:write("[showcase] saved " .. path .. " (" .. size .. " bytes)\n")
end

local function show_only(idx)
    for i, c in ipairs(clients) do
        if c.valid then
            c.minimized = (i ~= idx)
        end
    end
end

local function show_all()
    for _, c in ipairs(clients) do
        if c.valid then c.minimized = false end
    end
end

runner.run_async(function()
    local ok, err = pcall(function()
        -- Light wallpaper so black shadows are clearly visible.
        pcall(function() require("gears.wallpaper").set("#e7e7e7") end)

        local spawned = 0
        client.connect_signal("request::manage", function(c)
            if c.class == APP_ID or c.instance == APP_ID then
                spawned = spawned + 1
                local ex = EXAMPLES[spawned]
                if ex then
                    c.corner_radius = { radius = ex.radii, enabled = true }
                    c.border_width = 0
                    c.shadow = {
                        enabled = true,
                        radius = ex.shadow.radius,
                        offset_x = 0,
                        offset_y = 0,
                        spread = ex.shadow.spread,
                        opacity = ex.shadow.opacity,
                        color = "#000000",
                        follow_corners = true,
                    }
                end
                table.insert(clients, c)
            end
        end)

        for _ = 1, #EXAMPLES do
            local pid = awful.spawn(BINARY)
            assert(type(pid) == "number" and pid > 0, "failed to spawn pattern client")
            table.insert(pids, pid)
        end

        -- Wait for all five to appear and settle.
        async.wait_for_condition(function() return #clients >= #EXAMPLES end, 10, 0.05)
        async.sleep(1.0)

        -- Lay them out in a row (each 200x170, shadows extend beyond).
        local w, h = 200, 170
        local y = 270
        local xs = { 30, 270, 510, 750, 990 }
        for i, c in ipairs(clients) do
            c.floating = true
            c:geometry { x = xs[i], y = y, width = w, height = h }
        end
        async.sleep(0.6)

        io.stderr:write(("[showcase] wallpaper set; %d clients laid out\n"):format(#clients))

        -- Overview: everything visible.
        show_all()
        async.sleep(0.4)
        capture(DOCS_DIR .. "/shadow-corners-overview.png")

        -- One focused shot per example.
        for i, ex in ipairs(EXAMPLES) do
            show_only(i)
            local c = clients[i]
            if c.valid then
                c.floating = true
                c:geometry { x = 540, y = 275, width = w, height = h }
            end
            async.sleep(0.6)
            capture(string.format("%s/shadow-corners-%s.png", DOCS_DIR, ex.name))
            show_all()
        end

        io.stderr:write("[showcase] all captures written under " .. DOCS_DIR .. "/\n")
    end)

    for _, pid in ipairs(pids) do
        os.execute("kill -9 " .. pid .. " 2>/dev/null")
    end
    for _, c in ipairs(client.get()) do c:kill() end
    async.wait_for_no_clients(3)

    if not ok then
        runner.done("demo-shadow-corners: " .. tostring(err))
    else
        runner.done()
    end
end)