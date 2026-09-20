--[[
Autocolor: dominant-color sampling + reporting for SomeWM clients.

Derives a UI color from each client's own content (`client.content`, already
supported by the compositor) and reports it back through the callbacks the
caller installs (`M.bind`). The module itself renders nothing; the config owns
the widgets and the render/foreground wiring.

  * Modes: off | static | live.
      - `static` samples once when the client is first mapped (bounded retries).
      - `live` re-samples on the client's own surface commits. The compositor
        emits a per-client `surface::commit` signal (new hook in window.c, via
        the deferred event queue) so updates are EVENT-DRIVEN: idle clients do
        no work, and UNFOCUSED clients keep updating. Each commit also grants a
        small trailing budget of pump samples so XWayland clients whose scene
        snapshot is stale (e.g. focused Firefox tab switches) still converge.
        If a build predates the hook (no such signal), the module falls back
        to interval polling so live still works.
  * Exact color fidelity: the committed color is used exactly as sampled. No
    tint, no dimming, no desaturation. Focus distinction is the caller's
    concern (its own outline / focus colors) and/or the optional alpha, never
    by altering the sampled color.
  * Anti-flicker: a candidate is committed only when it differs from the
    committed color by more than `autocolor_threshold` CIELAB deltaE.
  * Readability: `M.foreground()` picks a light/dark color from WCAG-style
    linear luminance; the caller forwards it to its own text/widgets through
    the `fg_cb` callback.
  * Alpha: `autocolor_alpha` (0..1, default 1) blends the sampled color toward
    a caller-provided base color (typically the config's theme fill), giving a
    translucent surface without changing the hue. 1.0 = exact sampled color.
  * Fallback: on any read error, empty surface, early frame, or ambiguous
    histogram (`min_share`), the bound fallback is kept.

Config chain (rule -> theme -> fallback), per-client read via `custom_ac_*`:
  client.custom_ac_mode / custom_ac_interval / custom_ac_threshold /
  custom_ac_debounce / custom_ac_region /
  custom_ac_thumb / custom_ac_min_share / custom_ac_alpha /
  custom_ac_focus_outline ...
  beautiful.autocolor_* theme equivalents; legacy
  client.custom_tb_auto_color=true maps to "live" ("off" otherwise).
  beautiful.autocolor_white_fg (theme-only) overrides the RGB of the light
  "white" fg that `M.foreground()` derives (default #f2f2f2).
Default mode is "off"; enable per client.

Sampling cost: one `client:dominant_color()` call per sample on the main
thread, bounded per client and gated by `autocolor_interval`. The compositor
composites only the sampled strip (top rows, or a thumb-sized step grid over
the whole content for `region = "full"`) and runs a dependency-free C
histogram on it; there is no temp PNG and no Lua per-pixel loop. Once a
`surface::commit` has been observed, live clients are only sampled on commit
(plus a small trailing budget); idle clients do no readback.
]]

local gears = require("gears")
local beautiful = require("beautiful")

local M = {}

-- Per-client state (weak keys so closed clients are collected).
local STATE = setmetatable({}, { __mode = "k" })
local INITED = false

-- Set once the compositor has delivered any `surface::commit` signal, proving
-- this build has the hook. Until then the pump keeps its legacy every-interval
-- polling so builds without the hook still track changes.
local HOOK_SEEN = false

-- Trailing samples granted per commit; covers snapshots that are stale when the
-- commit-triggered sample runs (XWayland, e.g. focused Firefox tab switches).
local TRAILING_BUDGET = 2

local RETRY_DELAY = 1.0

-- (internal) unit-test seam: when set, cfg() returns this instead of reading
-- the client / beautiful chain.
local _cfg_override = nil

-- ---------------------------------------------------------------------------
-- Config
-- ---------------------------------------------------------------------------

local function opt(c, key, theme, dflt)
    local v
    if c then v = c["custom_ac_" .. key] end
    if v == nil then v = beautiful[theme] end
    if v == nil then return dflt end
    return v
end

function M.mode(c)
    local m = c and c.custom_ac_mode
    if m == nil and c and c.custom_tb_auto_color ~= nil then
        m = c.custom_tb_auto_color and "live" or "off"
    end
    if m == nil then m = beautiful.autocolor_mode end
    if m == nil and beautiful.titlebar_custom_auto_color ~= nil then
        m = beautiful.titlebar_custom_auto_color and "live" or "off"
    end
    if m == "static" or m == "live" then return m end
    return "off"
end

function M.is_active(c)
    return c and c.valid and M.mode(c) ~= "off"
end

local function cfg(c)
    if _cfg_override then return _cfg_override end
    return {
        mode         = M.mode(c),
        interval     = tonumber(opt(c, "interval", "autocolor_interval", 1)) or 1,
        threshold    = tonumber(opt(c, "threshold", "autocolor_threshold", 18)) or 18,
        debounce     = math.max(1, math.floor(tonumber(opt(c, "debounce", "autocolor_debounce", 2)) or 2)),
        region       = opt(c, "region", "autocolor_region", "top") == "top" and "top" or "full",
        thumb        = math.max(8, math.min(256, tonumber(opt(c, "thumb", "autocolor_thumb", 48)) or 48)),
        min_share    = tonumber(opt(c, "min_share", "autocolor_min_share", 0.06)) or 0.06,
        alpha        = math.max(0, math.min(1, tonumber(opt(c, "alpha", "autocolor_alpha", 1)) or 1)),
        retries      = math.max(0, math.floor(tonumber(opt(c, "retries", "autocolor_retries", 30)) or 30)),
        first_delay  = tonumber(opt(c, "first_delay", "autocolor_first_delay", 0.25)) or 0.25,
    }
end

function M.cfg(c)
    return c and cfg(c)
end

-- ---------------------------------------------------------------------------
-- Color helpers
-- ---------------------------------------------------------------------------

local function rgb(hex)
    if type(hex) ~= "string" then return nil end
    local h = hex:gsub("#", "")
    if #h == 3 then h = h:gsub(".", function(s) return s:rep(2) end) end
    if #h ~= 6 then return nil end
    local r, g, b = tonumber(h:sub(1, 2), 16), tonumber(h:sub(3, 4), 16), tonumber(h:sub(5, 6), 16)
    if not (r and g and b) then return nil end
    return r, g, b
end

local function lin(c8)
    local c = c8 / 255
    return c <= 0.04045 and c / 12.92 or ((c + 0.055) / 1.055) ^ 2.4
end

local function to_lab(r, g, b)
    local R, G, B = lin(r), lin(g), lin(b)
    local x = (R * 0.4124 + G * 0.3576 + B * 0.1805) / 0.95047
    local y = (R * 0.2126 + G * 0.7152 + B * 0.0722)
    local z = (R * 0.0193 + G * 0.1192 + B * 0.9505) / 1.08883
    local f = function(t) return t > 0.008856 and t ^ (1 / 3) or (7.787 * t + 16 / 116) end
    return { l = 116 * f(y) - 16, a = 500 * (f(x) - f(y)), b = 200 * (f(y) - f(z)) }
end

-- CIELAB deltaE; invalid colors are treated as equal (no change).
local function delta_e(a, b)
    local A, B = rgb(a) and to_lab(rgb(a)) or nil, rgb(b) and to_lab(rgb(b)) or nil
    if not A or not B then return 0 end
    local dl, da, db = A.l - B.l, A.a - B.a, A.b - B.b
    return math.sqrt(dl * dl + da * da + db * db)
end

-- Blend `hex` toward `base` by `t` (t=1 -> hex unchanged, t=0 -> base).
local function mix_to(hex, base, t)
    local r, g, b = rgb(hex)
    local br, bg, bb = rgb(base)
    if not (r and br) then return hex end
    local function cl(v) return math.max(0, math.min(255, math.floor(v + 0.5))) end
    return ("#%02x%02x%02x"):format(
        cl(r + (br - r) * (1 - t)), cl(g + (bg - g) * (1 - t)), cl(b + (bb - b) * (1 - t)))
end

-- Foreground picked for contrast; caller decides what it drives.
local LUM_THRESHOLD = 0.45

-- RGB override for the light ("white") fg, from beautiful.autocolor_white_fg.
-- Accepts hex (#RGB/#RRGGBB) or rgb()/rgba(); only the RGB is used (alpha, if
-- any, is dropped — the caller's focus/unfocus alphas decide translucency).
-- Returns nil when unset or unparseable, so the caller falls back to #f2f2f2.
local function white_fg()
    local v = beautiful.autocolor_white_fg
    if type(v) ~= "string" then return nil end
    if v:sub(1, 1) == "#" then
        local r, g, b = rgb(v)
        if not r then return nil end
        return ("#%02x%02x%02x"):format(r, g, b)
    end
    local r, g, b = gears.color.parse_color(v)
    if not r then return nil end
    local function cl(x) return math.max(0, math.min(255, math.floor(x * 255 + 0.5))) end
    return ("#%02x%02x%02x"):format(cl(r), cl(g), cl(b))
end

function M.foreground(hex)
    local r, g, b = rgb(hex)
    if not r then return "#f2f2f2" end
    local lum = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    if lum >= LUM_THRESHOLD then return "#181818" end
    return white_fg() or "#f2f2f2"
end

-- Focus outline derived from the sampled color's luminance so it stays visible
-- on both light and dark fills. Overridable per client / theme. Keeps the
-- sampled color untouched.
function M.outline_color(c, hex)
    local override = c and c.custom_ac_focus_outline or beautiful.autocolor_focus_outline
    if type(override) == "string" and override:sub(1, 1) == "#" then return override end
    local r, g, b = rgb(hex)
    if not r then return "#1a1a1a" end
    local lum = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    return lum >= LUM_THRESHOLD and "#1a1a1a" or "#e6e6e6"
end

-- Color handed to the bound render callback: the exact sampled color,
-- optionally blended toward the caller-provided base color by autocolor_alpha.
-- No-op when autocolor is off for the client or alpha is 1.0.
function M.render_color(c, hex, base)
    if not M.is_active(c) then return hex end
    local a = cfg(c).alpha
    if a >= 1 or not hex then return hex end
    return mix_to(hex, base, a)
end

-- Committed color for a client (nil if none yet / mode off).
function M.color_of(c)
    local st = STATE[c]
    return st and st.color
end

-- ---------------------------------------------------------------------------
-- Extraction (dominant color of the client content)
-- ---------------------------------------------------------------------------

-- Warn once per session when the running compositor build predates the
-- `client:dominant_color()` method (an older compositor). The bound fallback
-- color is kept and sampling is disabled.
local WARNED_MISSING = false

-- Sample the dominant color of a client's content. Returns hex, share, or
-- nothing. The heavy lifting runs inside the compositor:
--   * top   -> the top 12 rows across the full width
--   * full  -> the whole content, stepped so roughly thumb x thumb pixels
--              vote (the compositor derives the steps from the content size)
--   * min_share is applied here exactly as before: an ambiguous histogram
--              keeps the current color.
local function sample(c, _st)
    return pcall(function()
        local f = cfg(c)
        if not c.dominant_color then
            if not WARNED_MISSING then
                WARNED_MISSING = true
                gears.debug.print_warning(
                    "[autocolor] compositor lacks client:dominant_color(); " ..
                    "keeping the fallback color for all clients")
            end
            return nil
        end
        local opts = {
            rows = f.region == "top" and 12 or 0,
            bits = 4,
            min_alpha = 32,
        }
        if f.region == "full" then
            -- thumb is an approximate sample-grid size: roughly thumb x thumb
            -- pixels vote. The compositor picks step_x/step_y from the content
            -- size and thumb.
            opts.thumb = f.thumb
        end
        local hex, share = c:dominant_color(opts)
        if not hex or not share then return nil end
        if share < f.min_share then return nil end
        return hex, share
    end)
end

-- (internal) unit-test seam: run the sample logic with an explicit config so
-- the top/full -> options mapping and min_share gating are testable without a
-- real client.
function M._sample_for_tests(c, f)
    local prev_cfg = _cfg_override
    _cfg_override = f
    local ok, hex, share = sample(c, nil)
    _cfg_override = prev_cfg
    if not ok then return nil, nil end
    return hex, share
end

-- (internal) unit-test seam: clear the once-per-session missing-method warning.
function M._reset_warning_for_tests()
    WARNED_MISSING = false
end

-- ---------------------------------------------------------------------------
-- Commit pipeline (instant commit)
-- ---------------------------------------------------------------------------

local function commit(c, st, hex)
    if not c.valid then return end
    st.color = hex
    st.pending = nil
    if st.render then
        st.render(hex)
        if st.fg_cb then st.fg_cb(M.foreground(hex)) end
    end
end

local function on_sample(c, st, hex, share)
    if share < st.cfg.min_share then return end -- ambiguous: keep current

    if st.mode == "static" then
        if not st.color then commit(c, st, hex) end
        return
    end

    -- live: the committed color follows the sampled dominant IMMEDIATELY.
    -- The deltaE threshold is the only anti-flicker guard: near-equal colors
    -- are ignored, so a stable bar never jitters; a real content-color change
    -- lands on the next commit.
    if not st.color then
        commit(c, st, hex)
        return
    end
    if delta_e(st.color, hex) < st.cfg.threshold then
        return
    end
    commit(c, st, hex)
end

-- ---------------------------------------------------------------------------
-- Live sampling driver (commit-driven, with polling fallback)
-- ---------------------------------------------------------------------------

local function attempt(c, st, tries)
    if not c.valid then return end
    st.last_attempt = os.time()
    local ok, hex, share = sample(c, st)
    if ok and hex and share then
        on_sample(c, st, hex, share)
    elseif tries > 0 and not st.color then
        gears.timer.start_new(RETRY_DELAY, function() attempt(c, st, tries - 1) end)
    elseif tries <= 0 then
        gears.debug.print_warning("[autocolor] no color for " .. tostring(c))
    end
end

-- Gate: min gap between two extractions so a commit burst cannot thrash the
-- readback path. Commit-driven refreshes answer within this floor, bounded to
-- 1s even for large intervals; the interval pump sets the long-run cadence.
-- (os.time() is this fork's wall clock, second granularity.)
local function gate_ok(st)
    local n = os.time()
    if not st.last_attempt then return true end
    local gap = math.min(st.cfg.interval * 0.5, 1.0)
    return (n - st.last_attempt) >= gap
end

local function poll(c, st)
    if not c.valid then return end
    st.dirty = false
    st.last_attempt = os.time()
    local ok, hex, share = sample(c, st)
    if ok and hex and share then on_sample(c, st, hex, share) end
end

-- Request one extraction soon. Coalesced: while scheduled, additional commits
-- only set the dirty latch.
local schedule = function(c, st)
    if not c.valid then return end
    if st.scheduled then return end
    if not gate_ok(st) then st.dirty = true; return end
    st.scheduled = true
    gears.timer.delayed_call(function()
        st.scheduled = false
        poll(c, st)
    end)
end

local function on_commit(c)
    HOOK_SEEN = true
    local st = STATE[c]
    if not st or st.mode ~= "live" then return end
    st.commit_seen = true
    st.budget = TRAILING_BUDGET
    st.dirty = true
    schedule(c, st)
end

-- Pure pump decision. `hook_seen` is false until the compositor has delivered
-- any `surface::commit` (legacy builds without the hook never fire it), so the
-- pump keeps its old every-interval polling there. Once the hook is live,
-- sample only when the client is dirty or still within its post-commit
-- trailing budget; idle clients do no readback.
function M._pump_should_poll(st, hook_seen)
    if not hook_seen then return true end
    if st.dirty then return true end
    return (st.budget or 0) > 0
end

local function connect_surface(c, st)
    if st.sig then return end
    -- This fork's connect_signal connects successfully but its return value is
    -- not a usable connection id (it is falsy). Track the callback ourselves
    -- so disconnect_surface can pass it back by name.
    st.fn = function() on_commit(c) end
    local ok = pcall(function() c:connect_signal("surface::commit", st.fn) end)
    st.sig = ok
end

local function disconnect_surface(c, st)
    if st.sig and st.fn then
        pcall(function() c:disconnect_signal("surface::commit", st.fn) end)
    end
    st.sig = nil
    st.fn = nil
end

local function ensure_sampling(c, st)
    if st.started then return end
    st.started = true

    -- First paint (bounded retries); also establishes whether the compositor
    -- build has the commit hook (legacy builds never fire the signal).
    gears.timer.start_new(st.cfg.first_delay, function()
        attempt(c, st, st.cfg.retries)
    end)

    if st.mode ~= "live" then return end

    connect_surface(c, st)

    -- Pump. Once any `surface::commit` has been seen (HOOK_SEEN), sampling is
    -- event-driven: the pump only runs for a client that is dirty (a commit
    -- latched) or still within its post-commit trailing budget, which covers
    -- XWayland clients whose scene snapshot is stale (focused Firefox tab
    -- switches) until it converges. Builds without the hook never set HOOK_SEEN,
    -- so the pump keeps its legacy every-interval polling. Independent of
    -- focus: a mapped client always updates its own titlebar.
    local t = gears.timer { timeout = st.cfg.interval }
    t:connect_signal("timeout", function()
        if not c.valid then t:stop(); return end
        if st.dirty and not gate_ok(st) then return end
        if not M._pump_should_poll(st, HOOK_SEEN) then return end
        -- A budget-driven (trailing) sample consumes one unit of the budget;
        -- dirty samples are the commit response and leave the budget intact.
        if not st.dirty and st.budget > 0 then st.budget = st.budget - 1 end
        poll(c, st)
    end)
    t:start()
    st.live_t = t
end

-- ---------------------------------------------------------------------------
-- Lifecycle + public API
-- ---------------------------------------------------------------------------

-- (internal) ensure + return the per-client state
function M._ensure(c)
    local st = STATE[c]
    if not st then
        st = {
            mode = M.mode(c), cfg = cfg(c),
            live_t = nil, sig = nil,
            started = false, scheduled = false,
            commit_seen = false,
            budget = 0,
            dirty = false, last_attempt = nil,
            color = nil,
        }
        STATE[c] = st
    end
    return st
end

-- Bind a renderer for this client. `render(color)` is called at each committed
-- color; `fg_cb(hex)` with the readable foreground (`M.foreground`, typically
-- driving the caller's text/glyphs).
-- Returns the current committed color or the fallback.
function M.bind(c, fallback, render, fg_cb)
    if not M.is_active(c) then return fallback end
    local st = M._ensure(c)
    st.render = render
    st.fg_cb = fg_cb
    ensure_sampling(c, st)
    if st.color then
        st.render(st.color)
        if st.fg_cb then st.fg_cb(M.foreground(st.color)) end
    end
    return st.color or fallback
end

local function on_manage(c)
    if not M.is_active(c) then return end
    ensure_sampling(c, M._ensure(c))
end

local function on_unmanage(c)
    local st = STATE[c]
    if st then
        if st.live_t then st.live_t:stop() end
        disconnect_surface(c, st)
        STATE[c] = nil
    end
end

-- Wire client lifecycle. Idempotent; safe to call from rc.lua.
function M.setup()
    if INITED then return M end
    INITED = true

    client.connect_signal("request::manage", on_manage)
    client.connect_signal("request::unmanage", on_unmanage)

    return M
end

return M