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
        no work, and UNFOCUSED clients keep updating. If a build predates the
        hook (no such signal), the module falls back to interval polling so
        live still works.
  * Exact color fidelity: the committed color is used exactly as sampled. No
    tint, no dimming, no desaturation. Focus distinction is the caller's
    concern (its own outline / focus colors) and/or the optional alpha, never
    by altering the sampled color.
  * Anti-flicker: a candidate is committed only after `debounce` consecutive
    agreeing samples and only when it differs from the committed color by more
    than `autocolor_threshold` CIELAB deltaE. No sampling happens mid-fade.
  * Fade: `awesome.start_animation(fade, ease-out-cubic)` lerps RGB between the
    committed colors; steps lerp continuously across the full 256-level hex
    range, so consumer asset rebuilds only re-run on a committed change.
  * Readability: `M.foreground()` picks a light/dark color from WCAG-style
    linear luminance; the caller forwards it to its own text/widgets through
    the `fg_cb` callback.
  * Alpha: `autocolor_alpha` (0..1, default 1) blends the sampled color toward
    a caller-provided base color (typically the config's theme fill), giving a
    translucent surface without changing the hue. 1.0 = exact sampled color.
  * Fallback: on any read error, empty surface, early frame, or ambiguous
    histogram (`min_share`), the bound fallback is kept.

Config chain (rule -> theme -> fallback), per-client read via `custom_ac_*`:
  client.custom_ac_mode / custom_ac_interval / custom_ac_fade /
  custom_ac_threshold / custom_ac_debounce / custom_ac_region /
  custom_ac_thumb / custom_ac_min_share / custom_ac_alpha /
  custom_ac_focus_outline ...
  beautiful.autocolor_* theme equivalents; legacy
  client.custom_tb_auto_color=true maps to "live" ("off" otherwise).
Default mode is "off"; enable per client.

Sampling cost: one `c.content` readback per poll on the main thread, bounded per
client and gated by `autocolor_interval`; never during a fade. The compositor
readback path handles both SHM (zero-copy) and DMA-BUF/GPU (Firefox).
]]

local gears = require("gears")
local beautiful = require("beautiful")
local lgi = require("lgi")
local cairo = lgi.cairo

local M = {}

-- Per-client state (weak keys so closed clients are collected).
local STATE = setmetatable({}, { __mode = "k" })
local INITED = false

local RETRY_DELAY = 1.0

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
    return {
        mode         = M.mode(c),
        interval     = tonumber(opt(c, "interval", "autocolor_interval", 1)) or 1,
        fade         = tonumber(opt(c, "fade", "autocolor_fade", 0.35)) or 0.35,
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

local function lerp_hex(a, b, t)
    local ar, ag, ab = rgb(a)
    local br, bg, bb = rgb(b)
    if not ar or not br then return b end
    -- Intermediate steps lerp continuously across the full 256-level hex range
    -- (8 bits per channel); the FIFO cache only sees committed color changes.
    local function ic(v) return math.max(0, math.min(255, math.floor(v + 0.5))) end
    return ("#%02x%02x%02x"):format(
        ic(ar + (br - ar) * t), ic(ag + (bg - ag) * t), ic(ab + (bb - ab) * t))
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

function M.foreground(hex)
    local r, g, b = rgb(hex)
    if not r then return "#f2f2f2" end
    local lum = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    return lum >= LUM_THRESHOLD and "#181818" or "#f2f2f2"
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
-- Extraction (dominant color of c.content)
-- ---------------------------------------------------------------------------

local function mode_from_surface(surf, src_w, src_h, c, r)
    local f = cfg(c)
    local sample_w, sample_h
    if f.region == "top" then
        sample_w = math.max(1, math.min(math.floor(src_w / 2), src_w))
        sample_h = math.max(1, math.min(12, src_h))
    else
        local s = f.thumb / math.max(src_w, src_h, 1)
        sample_w = math.max(1, math.floor(src_w * s))
        sample_h = math.max(1, math.floor(src_h * s))
    end

    local ok_crop, crop = pcall(function()
        local cs = cairo.ImageSurface(cairo.Format.ARGB32, sample_w, sample_h)
        local cr = cairo.Context(cs)
        -- Nearest neighbor: bilinear would blend a dark glyph with the light
        -- bar around it and skew the dominant-color tally; nearest keeps the
        -- per-pixel colors that this module balances.
        cr:set_source_surface(surf, 0, 0)
        local pat = cr:get_source()
        if pat and pat.set_filter then pat:set_filter(cairo.Filter.NEAREST) end
        cr:scale(sample_w / src_w, sample_h / src_h)
        cr:paint()
        return cs
    end)
    if not ok_crop or not crop then return nil end

    -- In-memory readback: composite the cairo thumb into a GdkPixbuf directly
    -- (`gdk_pixbuf_get_from_surface`). No temp file, no PNG encode/decode,
    -- no FIFO-temp-name collision risk. GdkPixbuf stays loaded lazily, so the
    -- display-connection-early risk keeps out of this path (see get_images).
    local ok_g, GdkPixbuf = pcall(lgi.require, "GdkPixbuf", "2.0")
    if not ok_g then return nil end

    local ok_p, pixbuf = pcall(function()
        return GdkPixbuf.Pixbuf.get_from_surface(crop, 0, 0, sample_w, sample_h)
    end)
    if not ok_p or not pixbuf then return nil end

    local ok_px, px = pcall(function() return pixbuf:get_pixels() end)
    local ok_st, stride = pcall(function() return pixbuf:get_rowstride() end)
    local ok_nc, nch = pcall(function() return pixbuf:get_n_channels() end)
    if not ok_px or type(px) ~= "string" or not ok_st or not ok_nc or stride < 1 or nch < 3 then
        return nil
    end

    -- ROI mapped into thumbnail coordinates (nil r = whole thumbnail).
    local x0, x1, y0, y1 = 0, sample_w, 0, sample_h
    if r then
        x0 = math.max(0, math.floor(r.x * sample_w / src_w))
        x1 = math.min(sample_w, math.ceil((r.x + r.w) * sample_w / src_w))
        y0 = math.max(0, math.floor(r.y * sample_h / src_h))
        y1 = math.min(sample_h, math.ceil((r.y + r.h) * sample_h / src_h))
    end

    local tally, mode, mode_count, total, checked = {}, nil, 0, 0, 0
    for x = x0, x1 - 1, 2 do
        for y = y0, y1 - 1 do
            local off = y * stride + x * nch
            checked = checked + 1
            local r_, g_, b_ = px:byte(off + 1, off + 3)
            -- Skip transparent pixels. A `c.content` snapshot whose buffers
            -- were absent during a rapid scene reorder composites nothing and
            -- comes back fully transparent (ARGB 0x00000000); sampling RGB
            -- alone would read that as a "black" dominant and flip the bar
            -- black. Transparent pixels carry no visible color, so they must
            -- not vote.
            local a_ = nch >= 4 and px:byte(off + 4) or 255
            if r_ and g_ and b_ and a_ and a_ >= 32 then
                local color = ("#%02x%02x%02x"):format(r_, g_, b_)
                total = total + 1
                local n = (tally[color] or 0) + 1
                tally[color] = n
                if n > mode_count then mode, mode_count = color, n end
            end
        end
    end
    -- An empty or mostly-transparent snapshot carries no usable color, same
    -- fallback contract as a failed readback.
    if checked == 0 or total < f.min_share * checked then return nil end
    return mode, mode_count / total
end

-- Cheap path: `c.content` (client scene subtree, content minus decorations).
-- Cost is O(window pixels) for the readback; fine at a bounded cadence.
local function extract(c)
    local ok_c, content = pcall(function() return c.content end)
    if not ok_c or not content then return nil end
    local ok_s, s = pcall(gears.surface, content)
    if not ok_s then return nil end
    local ok_w, w = pcall(function() return s:get_width() end)
    local ok_h, h = pcall(function() return s:get_height() end)
    if not (ok_w and ok_h) or type(w) ~= "number" or type(h) ~= "number" or w < 1 or h < 1 then
        return nil
    end
    return mode_from_surface(s, w, h, c)
end

local function sample(c, _st)
    return pcall(extract, c)
end

-- ---------------------------------------------------------------------------
-- Commit pipeline (instant commit -> fade)
-- ---------------------------------------------------------------------------

-- Forward declarations: `commit` runs before `fade` is defined; `fade`'s
-- completion handler calls `schedule`, also defined later in this chunk.
local fade, schedule

local function commit(c, st, hex)
    if not c.valid then return end
    st.color = hex
    st.pending = nil
    fade(c, st, st.shown or st.fallback, hex)
end

local function on_sample(c, st, hex, share)
    if share < st.cfg.min_share then return end -- ambiguous: keep current

    if st.mode == "static" then
        if not st.color then commit(c, st, hex) end
        return
    end

    -- live: the committed color follows the sampled dominant IMMEDIATELY.
    -- The deltaE threshold is the only anti-flicker guard: near-equal colors
    -- are ignored and the fade smooths the step, so a stable bar never
    -- jitters, but a real content-color change lands on the next commit.
    if not st.color then
        commit(c, st, hex)
        return
    end
    if delta_e(st.color, hex) < st.cfg.threshold then
        return
    end
    commit(c, st, hex)
end

fade = function(c, st, from, to)
    local rd = st.render
    if not rd then return end
    if st.handle then pcall(function() st.handle:cancel() end); st.handle = nil end

    -- Foreground latch starts at the settled foreground so mid-fade we only
    -- emit on an actual flip (titlebar text flips exactly when it should).
    local fg_latch = M.foreground(from or to)

    local function settle()
        st.shown, st.color = to, to
        rd(to)
        if st.fg_cb then st.fg_cb(M.foreground(to)) end
    end

    local dur = st.cfg.fade
    if not from or from == to or dur <= 0 then settle(); return end

    st.handle = awesome.start_animation(dur, "ease-out-cubic",
        function(t)
            local h = lerp_hex(from, to, t)
            st.shown = h
            rd(h)
            -- Emit the caller's foreground on every step where the preferred
            -- candidate flips, not only on settle: title text stays readable
            -- while the bar itself is still mid-fade.
            local fg = M.foreground(h)
            if st.fg_cb and fg ~= fg_latch then
                fg_latch = fg
                st.fg_cb(fg)
            end
        end,
        function()
            st.handle = nil
            if not c.valid then return end
            settle()
            -- Content changed while the fade was running: sample once more.
            if st.mode == "live" and st.dirty then schedule(c, st) end
        end)
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
-- only set the dirty latch. Binds the forward-declared upvalue from the
-- commit-pipeline section (fade's completion handler also calls it).
schedule = function(c, st)
    if not c.valid then return end
    if st.scheduled then return end
    if st.handle then st.dirty = true; return end -- mid-fade: wait
    if not gate_ok(st) then st.dirty = true; return end
    st.scheduled = true
    gears.timer.delayed_call(function()
        st.scheduled = false
        poll(c, st)
    end)
end

local function on_commit(c)
    local st = STATE[c]
    if not st or st.mode ~= "live" then return end
    st.commit_seen = true
    st.dirty = true
    schedule(c, st)
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

    -- Pump. Always samples each interval so XWayland clients whose scene
    -- snapshot goes stale (focused Firefox tab switches) still track changes;
    -- the commit hook supplies extra immediacy on top for native clients.
    -- Independent of focus: a mapped client always updates its own titlebar.
    local t = gears.timer { timeout = st.cfg.interval }
    t:connect_signal("timeout", function()
        if not c.valid then t:stop(); return end
        if st.handle then return end          -- never poll mid-fade
        if st.dirty and not gate_ok(st) then return end
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
            handle = nil, live_t = nil, sig = nil,
            started = false, scheduled = false,
            commit_seen = false,
            dirty = false, last_attempt = nil,
            color = nil, shown = nil,
        }
        STATE[c] = st
    end
    return st
end

-- Bind a renderer for this client. `render(color)` is called at each committed
-- color (and fade step); `fg_cb(hex)` on settle with the readable foreground
-- (`M.foreground`, typically driving the caller's text/glyphs).
-- Returns the current committed color or the fallback.
function M.bind(c, fallback, render, fg_cb)
    if not M.is_active(c) then return fallback end
    local st = M._ensure(c)
    st.render = render
    st.fg_cb = fg_cb
    st.fallback = fallback or "#222222"
    ensure_sampling(c, st)
    if st.color then fade(c, st, st.shown or st.color, st.color) end
    return st.color or fallback
end

local function on_manage(c)
    if not M.is_active(c) then return end
    ensure_sampling(c, M._ensure(c))
end

local function on_unmanage(c)
    local st = STATE[c]
    if st then
        if st.handle then pcall(function() st.handle:cancel() end) end
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