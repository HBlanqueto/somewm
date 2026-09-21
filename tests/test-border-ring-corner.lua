---------------------------------------------------------------------------
--- Regression: a 1px border ring must not be chopped at a rounded corner.
---
--- SceneFX's apply_clip_region() shrinks a rounded clipped region by
--- 0.3 * radius before rasterizing. The ring's outer arc passes at
--- (1 - sqrt(2)/2) * radius from the cutout corner (~0.293 * r), so that
--- pixel was never drawn: the border showed a notch at ~45 degrees.
---
--- Captures the real output with grim (wlr-screencopy; root.content()'s
--- software path paints scene rects as plain rectangles and cannot see the
--- ring) and walks the corner arc: every row between the straight top border
--- and the straight left border must carry a border-colored pixel.
---
--- Run: make test-one TEST=tests/test-border-ring-corner.lua
---      (needs grim on PATH)
---------------------------------------------------------------------------

local awful  = require("awful")
local runner = require("_runner")
local async  = require("_async")

local RADIUS = 14
local BORDER = 1
local RED    = "#ff0000"

local function grim_available()
    local ok = os.execute("command -v grim >/dev/null 2>&1")
    return ok == 0 or ok == true
end

if not grim_available() then
    io.stderr:write("SKIP: grim not available (needed for wlr-screencopy)\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

-- Minimal P6 reader: grim writes "P6\n<w> <h>\n255\n" followed by raw RGB.
local function load_ppm(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read("*a")
    f:close()
    local w, h = data:match("^P6%s+(%d+)%s+(%d+)%s+255%s")
    local _, e = data:find("^P6%s+%d+%s+%d+%s+255%s")
    if not w or not e then return nil end
    return tonumber(w), tonumber(h), data:sub(e + 1)
end

local function pixel_rgb(pix, w, x, y)
    local o = (y * w + x) * 3 + 1
    return pix:byte(o), pix:byte(o + 1), pix:byte(o + 2)
end

-- The border color is opaque red; AA-blended ring pixels keep a clear red
-- lead over the green/blue channels.
local function is_border(pix, w, x, y)
    local r, g, b = pixel_rgb(pix, w, x, y)
    return r - math.max(g, b) > 40
end

local function capture(path)
    os.remove(path)
    awful.spawn.with_shell("grim -t ppm " .. path)
    async.wait_for_condition(function()
        local f = io.open(path, "r")
        if not f then return false end
        local size = f:seek("end")
        f:close()
        return size > 2000
    end, 10, 0.1)
    async.sleep(0.2)
    return load_ppm(path)
end

runner.run_async(function()
    local pid = awful.spawn("foot")
    assert(type(pid) == "number" and pid > 0, "failed to spawn foot")

    local c = async.wait_for_client("foot", 5)
    assert(c, "foot never appeared")

    c.floating = true
    c.border_width = BORDER
    c.corner_radius = { radius = RADIUS, enabled = true }
    c._border_color = RED
    c:geometry { x = 200, y = 150, width = 400, height = 300 }
    async.sleep(1.0)

    local path = os.tmpname()
    local w, h, pix = capture(path)
    os.remove(path)
    assert(pix, "grim capture failed")

    -- Locate the frame: the red bbox's top-left is the frame's outer corner.
    local minx, miny = w, h
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            if is_border(pix, w, x, y) then
                if x < minx then minx = x end
                if y < miny then miny = y end
            end
        end
    end
    assert(minx < w, "no border pixels found; border color did not apply")

    -- Walk the top-left arc: each row between the straight edges must have a
    -- ring pixel. With the bug one row (~45 degrees) is empty.
    local missing = {}
    for y = miny + 1, miny + RADIUS - 1 do
        local found = false
        for x = minx, minx + RADIUS do
            if is_border(pix, w, x, y) then found = true break end
        end
        if not found then missing[#missing + 1] = y end
    end

    io.stderr:write(string.format(
        "[border-ring] frame origin=%d,%d radius=%d border=%d missing rows=%s\n",
        minx, miny, RADIUS, BORDER,
        #missing > 0 and table.concat(missing, ",") or "none"))

    assert(#missing == 0,
        "border ring is chopped at the rounded corner: no border pixel in row(s) " ..
        table.concat(missing, ",") .. " of the arc")

    c:kill()
    async.wait_for_no_clients(3)
    runner.done()
end)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
