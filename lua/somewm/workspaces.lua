-- lua/somewm/workspaces.lua
--
-- macOS Spaces-style dynamic workspaces for SomeWM.
--
-- Each screen owns an ordered list of workspaces (the tag ORDER is the list
-- order: the slide and next/prev follow it). Two kinds:
--   * "desktop"  a numbered desktop ("Escritorio N"), named desktop_01..NN;
--   * "focus"    a focus space, a volatile tag created right after its origin
--                desktop, named <app>_<NN>, backdrop "black".
--
-- Desktops are numbered by position among desktops only (focus spaces don't
-- count) and renumbered on add/remove, like macOS "Desktop 1..n". Every
-- workspace has a stable id (ws_<monotonic counter>) that is never reused
-- within a session (the counter survives hot-reloads via the state file).
--
-- The layout (per-screen desktop order and ids) survives a hot-reload and a
-- restart through $XDG_STATE_HOME/somewm/workspaces.json (atomic writes; a
-- corrupt file is kept as workspaces.json.corrupt and a fresh layout starts).
-- Focus spaces are session-scoped but are persisted too so a hot-reload can
-- recreate them right after their origin and re-attach the focus-space client
-- (identified by client id) before the C retag maps old->new tags by name.
--
-- Exposed over the first piece of the versioned IPC v2 contract:
--   EVENT workspaces {version, screens:[{screen, workspaces:[...]}]}
--     broadcast on every model change (add, remove, renumber, rename,
--     selection, client moves);
--   workspace add|remove|view|view_desktop|list
-- `workspace list` re-broadcasts the snapshot (for (re)connects) and replies OK.
-- The snapshot maps 1:1 onto ext-workspace-v1 (workspace group = screen,
-- selected = active, removable = REMOVE capability), so it can later be
-- exposed as that protocol without changing semantics.

local awful = require("awful")

local M = {}

local VERSION = 1

-- ---------------------------------------------------------------------------
-- Compact JSON codec (self-contained: the fork has no JSON decoder).
-- ---------------------------------------------------------------------------

local json = {}

local json_escapes = {
    ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
    ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function json_escape(s)
    return (s:gsub('[%z\1-\31\\"]', function(ch)
        return json_escapes[ch] or ('\\u%04x'):format(ch:byte())
    end))
end

function json.encode(v)
    local t = type(v)
    if t == "nil" then
        return "null"
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif t == "number" then
        if v ~= v then return "null" end
        return tostring(v)
    elseif t == "string" then
        return '"' .. json_escape(v) .. '"'
    elseif t == "table" then
        -- Array? Sequential integer keys 1..n with no holes.
        local is_array, max_idx = true, 0
        for k in pairs(v) do
            if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then
                is_array = false
                break
            end
            if k > max_idx then max_idx = k end
        end
        if is_array then
            for i = 1, max_idx do
                if v[i] == nil then is_array = false break end
            end
        end
        if is_array and max_idx > 0 then
            local parts = {}
            for i = 1, max_idx do
                parts[#parts + 1] = json.encode(v[i])
            end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local parts = {}
        for k, val in pairs(v) do
            parts[#parts + 1] = json.encode(tostring(k)) .. ":" .. json.encode(val)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return '"' .. tostring(v) .. '"'
end

local function json_decode(str)
    local pos = 1
    local function skipws()
        local c = str:sub(pos, pos)
        while c == " " or c == "\t" or c == "\n" or c == "\r" do
            pos = pos + 1
            c = str:sub(pos, pos)
        end
    end
    local function parse()
        skipws()
        local ch = str:sub(pos, pos)
        if ch == "{" then
            pos = pos + 1
            local out = {}
            skipws()
            if str:sub(pos, pos) == "}" then pos = pos + 1 return out end
            while true do
                local k = parse()
                skipws()
                if str:sub(pos, pos) ~= ":" then error("expected ':'") end
                pos = pos + 1
                local v = parse()
                out[k] = v
                skipws()
                local c = str:sub(pos, pos)
                if c == "," then
                    pos = pos + 1
                elseif c == "}" then
                    pos = pos + 1
                    return out
                else
                    error("expected ',' or '}'")
                end
            end
        elseif ch == "[" then
            pos = pos + 1
            local out = {}
            skipws()
            if str:sub(pos, pos) == "]" then pos = pos + 1 return out end
            local i = 1
            while true do
                out[i] = parse()
                skipws()
                local c = str:sub(pos, pos)
                if c == "," then
                    pos = pos + 1
                elseif c == "]" then
                    pos = pos + 1
                    return out
                else
                    error("expected ',' or ']'")
                end
                i = i + 1
            end
        elseif ch == '"' then
            pos = pos + 1
            local buf = {}
            while true do
                local c = str:sub(pos, pos)
                if c == "" then error("unterminated string") end
                if c == '"' then
                    pos = pos + 1
                    return table.concat(buf)
                elseif c == "\\" then
                    pos = pos + 1
                    local e = str:sub(pos, pos)
                    if e == "n" then buf[#buf + 1] = "\n"
                    elseif e == "t" then buf[#buf + 1] = "\t"
                    elseif e == "r" then buf[#buf + 1] = "\r"
                    elseif e == "b" then buf[#buf + 1] = "\b"
                    elseif e == "f" then buf[#buf + 1] = "\f"
                    elseif e == "/" then buf[#buf + 1] = "/"
                    elseif e == "\\" then buf[#buf + 1] = "\\"
                    elseif e == '"' then buf[#buf + 1] = '"'
                    elseif e == "u" then
                        local hex = str:sub(pos + 1, pos + 4)
                        if #hex < 4 then error("bad \\u escape") end
                        local cp = tonumber(hex, 16)
                        if not cp or cp > 127 then error("non-ASCII \\u escape") end
                        buf[#buf + 1] = string.char(cp)
                        pos = pos + 4
                    else
                        error("bad escape: " .. tostring(e))
                    end
                    pos = pos + 1
                else
                    buf[#buf + 1] = c
                    pos = pos + 1
                end
            end
        elseif ch == "-" or (ch ~= "" and ch:match("%d")) then
            local num = str:match("[-+]?%d+%.?%d*[eE]?[+-]?%d*", pos)
            if not num then error("bad number") end
            pos = pos + #num
            return tonumber(num)
        elseif ch == "t" then
            if str:sub(pos, pos + 3) == "true" then pos = pos + 4 return true end
            error("bad literal")
        elseif ch == "f" then
            if str:sub(pos, pos + 4) == "false" then pos = pos + 5 return false end
            error("bad literal")
        elseif ch == "n" then
            if str:sub(pos, pos + 3) == "null" then pos = pos + 4 return nil end
            error("bad literal")
        end
        error("unexpected character")
    end
    local value = parse()
    skipws()
    if pos <= #str then error("trailing garbage") end
    return value
end

function json.decode(str)
    if type(str) ~= "string" then return nil end
    local ok, value = pcall(json_decode, str)
    if not ok then return nil end
    return value
end

-- ---------------------------------------------------------------------------
-- State
-- ---------------------------------------------------------------------------

-- Per-screen state, keyed by output connector name:
--   state.screens[conn] = {
--     desktops = { {id, tag}, ... },   -- ordered
--     focus    = { {id, name, label, app_key, client_id, origin_id, tag, client}, ... }
--   }
-- Tag -> entry reverse maps (strong refs keep a deleted tag's object alive so
-- .valid can be checked deterministically; dead entries are dropped by resync).
local state = { screens = {} }
local desktops_by_tag = {}
local focus_by_tag = {}

local next_id = 0
local state_dir = os.getenv("XDG_STATE_HOME")
    or (os.getenv("HOME") and os.getenv("HOME") .. "/.local/state")
    or "/tmp"
local last_serialized = nil

-- Rapid-switch queue: presses arriving while a slide runs are deferred and
-- replayed as chained (faster) slides after each one completes, so a burst
-- never cuts a slide short. Direction reversal drops the pending steps.
local step_queue = {}
local STEP_QUEUE_DEPTH = 4
local CHAIN_FACTOR = 0.6
local CHAIN_FLOOR = 0.15
local base_duration = nil

local function slide_last_duration()
    local dur = type(slide) == "table" and type(slide.duration) == "function"
        and slide.duration() or nil
    return type(dur) == "number" and dur or nil
end

local function slide_set_duration(d)
    if type(slide) == "table" and type(slide.set_duration) == "function" then
        pcall(slide.set_duration, d)
    end
end

local function slide_running()
    if type(slide) ~= "table" or type(slide.active) ~= "function" then
        return false
    end
    local ok, on = pcall(slide.active)
    return ok and on == true
end

local function state_file()
    return state_dir .. "/somewm/workspaces.json"
end

local function connector(s)
    local name = s and s.output and s.output.name
    if type(name) == "string" and name ~= "" then return name end
    return "screen" .. tostring(s and s.index or 0)
end

local function mkdir_p(dir)
    -- Single-quote for a path that cannot contain a quote (we own the dirs).
    pcall(os.execute, "mkdir -p '" .. dir:gsub("'", "'\\''") .. "'")
end

local function next_id_str()
    next_id = next_id + 1
    return ("ws_%d"):format(next_id)
end

local function id_number(id)
    return tonumber(id and id:match("^ws_(%d+)$")) or 0
end

-- Tags have no `valid` property; a deleted tag reads `activated == false`.
local function tag_alive(t)
    return t ~= nil and t.activated == true
end

local function default_layout()
    return awful.layout.layouts[1] or awful.layout.suit.floating
end

-- ---------------------------------------------------------------------------
-- State file
-- ---------------------------------------------------------------------------

local function persist()
    local data = { version = VERSION, next_id = next_id, screens = {} }
    for conn, se in pairs(state.screens) do
        local entry = { desktops = {}, focus = {} }
        for _, d in ipairs(se.desktops) do
            if tag_alive(d.tag) then
                entry.desktops[#entry.desktops + 1] = { id = d.id }
            end
        end
        for _, f in ipairs(se.focus) do
            if tag_alive(f.tag) then
                entry.focus[#entry.focus + 1] = {
                    id = f.id, name = f.name, label = f.label, app_key = f.app_key,
                    client_id = f.client_id, origin_id = f.origin_id,
                }
            end
        end
        data.screens[conn] = entry
    end
    local file = state_file()
    mkdir_p(state_dir .. "/somewm")
    local tmp = file .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return end
    f:write(json.encode(data), "\n")
    f:close()
    os.rename(tmp, file)
end

local function load_state()
    state = { screens = {} }
    next_id = 0
    local file = state_file()
    local f = io.open(file, "r")
    if not f then return end
    local content = f:read("*a")
    f:close()
    local data = json.decode(content)
    if not data or type(data.screens) ~= "table" then
        -- Unreadable: keep the file for inspection, start fresh.
        pcall(os.rename, file, file .. ".corrupt")
        return
    end
    if type(data.next_id) == "number" then next_id = data.next_id end
    for conn, se in pairs(data.screens) do
        if type(se) == "table" then
            local entry = { desktops = {}, focus = {} }
            if type(se.desktops) == "table" then
                for _, d in ipairs(se.desktops) do
                    if type(d) == "table" and type(d.id) == "string" then
                        entry.desktops[#entry.desktops + 1] = { id = d.id }
                    end
                end
            end
            if type(se.focus) == "table" then
                for _, fo in ipairs(se.focus) do
                    if type(fo) == "table" and type(fo.id) == "string" then
                        entry.focus[#entry.focus + 1] = {
                            id = fo.id,
                            name = type(fo.name) == "string" and fo.name or "app_01",
                            label = type(fo.label) == "string" and fo.label or "App",
                            app_key = type(fo.app_key) == "string" and fo.app_key or "app",
                            client_id = tonumber(fo.client_id) or -1,
                            origin_id = type(fo.origin_id) == "string" and fo.origin_id or "",
                        }
                    end
                end
            end
            state.screens[conn] = entry
            for _, d in ipairs(entry.desktops) do
                if id_number(d.id) >= next_id then next_id = id_number(d.id) end
            end
            for _, fo in ipairs(entry.focus) do
                if id_number(fo.id) >= next_id then next_id = id_number(fo.id) end
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Classification helpers
-- ---------------------------------------------------------------------------

local function find_client_by_id(id)
    for _, c in ipairs(client.get()) do
        if c.id == id then return c end
    end
    return nil
end

-- "org.wezfurlong.wezterm" -> "wezterm", "firefox" -> "firefox", fallback "app".
local function app_key(c)
    local cls = c and c.class or ""
    local tail = cls:match("([^%.]+)$") or cls
    local key = tail:lower():gsub("[^a-z0-9]", "")
    if key == "" then return "app" end
    return key
end

-- Display name for the UI: "org.wezfurlong.wezterm" -> "Wezterm".
local function app_label(c)
    local cls = c and c.class or ""
    local tail = cls:match("([^%.]+)$") or cls
    if tail == "" then tail = "App" end
    return tail:sub(1, 1):upper() .. tail:sub(2)
end

local function desktop_entry_for_tag(t)
    local d = desktops_by_tag[t]
    if d and tag_alive(d.tag) then return d end
    return nil
end

local function focus_entry_for_tag(t)
    local f = focus_by_tag[t]
    if f and tag_alive(f.tag) then return f end
    return nil
end

local function screen_state(s)
    local conn = connector(s)
    local se = state.screens[conn]
    if not se then
        se = { desktops = {}, focus = {} }
        state.screens[conn] = se
    end
    return se, conn
end

-- Rename desktops by position so names always read desktop_01..NN.
local function renumber(se)
    for i, d in ipairs(se.desktops) do
        local want = ("desktop_%02d"):format(i)
        if tag_alive(d.tag) and d.tag.name ~= want then
            d.tag.name = want
        end
    end
end

-- ---------------------------------------------------------------------------
-- Workspace operations
-- ---------------------------------------------------------------------------

-- Forward declarations: the signals below run before the definitions.
local changed
local resolve_tag

local function pick_remaining_screen(s)
    local focused = awful.screen.focused()
    if focused and focused ~= s then return focused end
    for other in screen do
        if other ~= s then return other end
    end
    return nil
end

-- Focus spaces keep living next to their origin desktop; after a removal the
-- placement follows focus_space.placement ("after_origin" sits right after the
-- re-pointed origin, "end" goes to the end of the list). Called after a
-- screen merge or a desktop removal so re-pointed spaces stay in place.
local function reindex_screen(s, placement)
    local se = state.screens[connector(s)]
    if not se then return end
    local desired = {}
    if placement == "end" then
        for _, d in ipairs(se.desktops) do
            if tag_alive(d.tag) then
                desired[#desired + 1] = d.tag
            end
        end
        for _, f in ipairs(se.focus) do
            if tag_alive(f.tag) then
                desired[#desired + 1] = f.tag
            end
        end
    else
        for _, d in ipairs(se.desktops) do
            if tag_alive(d.tag) then
                desired[#desired + 1] = d.tag
                for _, f in ipairs(se.focus) do
                    if f.origin_id == d.id and tag_alive(f.tag) then
                        desired[#desired + 1] = f.tag
                    end
                end
            end
        end
    end
    for i, t in ipairs(desired) do
        if tag_alive(t) and t.index ~= i then
            t.index = i
        end
    end
end

-- Desired focus-space placement after a desktop removal, from the config's
-- settings.json ("focus_space.placement"); defaults to after_origin.
local function focus_placement()
    local ok, settings = pcall(require, "core.settings")
    if ok and type(settings) == "table" and type(settings.get) == "function" then
        local fp = settings.get("focus_space")
        if type(fp) == "table" then
            local p = fp.placement
            if p == "after_origin" or p == "end" then return p end
        end
    end
    return "after_origin"
end

-- Workspace cap for a screen (desktops + focus spaces), read from the same
-- settings.json the widget consumes ("<workspaces>.max") so M.add and the
-- bar's add button never disagree. Defaults to 16 when absent.
local function workspaces_max()
    local ok, settings = pcall(require, "core.settings")
    if ok and type(settings) == "table" and type(settings.get) == "function" then
        local ws = settings.get("workspaces")
        if type(ws) == "table" and type(ws.max) == "number" and ws.max >= 1 then
            return math.floor(ws.max)
        end
    end
    return 16
end

-- ---------------------------------------------------------------------------
-- Signals
-- ---------------------------------------------------------------------------

local function on_tag_screen_removed(t, context)
    if context ~= "removed" then return end
    if not (tag_alive(t)) then return end
    local s = t.screen
    if not s then return end
    local remaining = pick_remaining_screen(s)
    if not remaining then return end
    -- The actual screen move: appended at the end of `remaining`'s list. The
    -- order is fixed up in on_screen_removed.
    pcall(function() t.screen = remaining end)
end

local function on_screen_removed(s)
    local conn = connector(s)
    local se = state.screens[conn]
    local remaining = pick_remaining_screen(s)
    if not se then return end
    if not remaining then return end

    local rconn = connector(remaining)
    local rse = state.screens[rconn] or { desktops = {}, focus = {} }
    state.screens[rconn] = rse

    -- Append the removed screen's desktops (with their windows) and focus
    -- spaces to the right end of the remaining screen's list, in order.
    for _, d in ipairs(se.desktops) do
        if tag_alive(d.tag) then rse.desktops[#rse.desktops + 1] = d end
    end
    for _, f in ipairs(se.focus) do
        if tag_alive(f.tag) then
            rse.focus[#rse.focus + 1] = f
            if f.client and f.client.valid then
                f.client.focus_space_screen = remaining
            end
        end
    end
    state.screens[conn] = nil

    reindex_screen(remaining)
    renumber(rse)
    changed(true)
end

-- ---------------------------------------------------------------------------
-- Snapshot + broadcast
-- ---------------------------------------------------------------------------

function M.snapshot()
    local out = { version = VERSION, screens = {} }
    for s in screen do
        local se = state.screens[connector(s)]
        local n_desktops = se and #se.desktops or 0
        local ws = {}
        for _, t in ipairs(s.tags) do
            local d = desktop_entry_for_tag(t)
            local f = focus_entry_for_tag(t)
            local w
            if d then
                local number = 0
                if se then
                    for i, de in ipairs(se.desktops) do
                        if de == d then number = i break end
                    end
                end
                w = {
                    id = d.id,
                    name = t.name,
                    label = ("Escritorio %d"):format(number),
                    kind = "desktop",
                    index = t.index,
                    desktop_number = number,
                    selected = t.selected,
                    client_count = #t:clients(),
                    removable = n_desktops > 1,
                }
            elseif f then
                w = {
                    id = f.id,
                    name = t.name,
                    label = f.label,
                    kind = "focus",
                    index = t.index,
                    desktop_number = nil,
                    selected = t.selected,
                    client_count = #t:clients(),
                    removable = true,
                }
            else
                -- A tag this module does not own: mirror it so the snapshot
                -- always reflects s.tags, but mark it non-removable.
                w = {
                    id = ("tag_%d"):format(t.index),
                    name = t.name,
                    label = t.name,
                    kind = "other",
                    index = t.index,
                    desktop_number = nil,
                    selected = t.selected,
                    client_count = #t:clients(),
                    removable = false,
                }
            end
            ws[#ws + 1] = w
        end
        out.screens[#out.screens + 1] = {
            screen = s.index,
            name = s.output and s.output.name or "",
            max = workspaces_max(),
            workspaces = ws,
        }
    end
    return out
end

function M.list()
    return json.encode(M.snapshot())
end

function M.broadcast(force)
    local snap = M.snapshot()
    local ser = json.encode(snap)
    if not force and ser == last_serialized then return end
    last_serialized = ser
    local ok, ipc = pcall(require, "awful.ipc")
    if ok and ipc and ipc.broadcast then
        ipc.broadcast("workspaces", snap)
    end
end

changed = function(structural)
    if structural then persist() end
    M.broadcast(false)
end

-- Re-sync the registries from live tags. Signal-driven: fires on client
-- tag/untag/unmanage and tag selection/backdrop changes. Dropping a deleted
-- focus tag is structural (persist); everything else just re-broadcasts.
local function resync()
    local structural = false
    for _, se in pairs(state.screens) do
        local live_desktops = {}
        for _, d in ipairs(se.desktops) do
            -- tag == nil: not materialized yet (module still loading); keep.
            if not d.tag or tag_alive(d.tag) then
                live_desktops[#live_desktops + 1] = d
            else
                desktops_by_tag[d.tag] = nil
                structural = true
            end
        end
        se.desktops = live_desktops
        local live_focus = {}
        for _, f in ipairs(se.focus) do
            if not f.tag or tag_alive(f.tag) then
                live_focus[#live_focus + 1] = f
            else
                focus_by_tag[f.tag] = nil
                structural = true
            end
        end
        se.focus = live_focus
    end
    changed(structural)
end

local function delayed_resync()
    -- leave() deletes the focus tag AFTER moving the client out, so the
    -- tagged/untagged signal alone can run too early; re-check once more.
    local ok, timer = pcall(require, "gears.timer")
    if ok and timer then
        timer.delayed_call(resync)
    end
end

-- ---------------------------------------------------------------------------
-- Focus-space seam (called by libs/focus_space.lua)
-- ---------------------------------------------------------------------------

-- Create the temporary tag for a focus space right after its origin desktop.
-- Names it <app>_<NN> (NN = lowest number free for that app on the screen,
-- stable while the space exists), sets the black backdrop, and registers the
-- space with the module so it appears in the model and survives a hot-reload.
-- Returns the new tag.
function M.enter_focus_space(c, origin, origin_index)
    if not (c and c.valid and tag_alive(origin)) then return nil end
    local screen = origin.screen
    if not screen then return nil end
    local se = screen_state(screen)

    local key = app_key(c)
    local used = {}
    for _, f in ipairs(se.focus) do
        if tag_alive(f.tag) and f.app_key == key then
            local nn = f.name:match("_(%d+)$")
            if nn then used[tonumber(nn)] = true end
        end
    end
    local nn = 1
    while used[nn] do nn = nn + 1 end
    local name = ("%s_%02d"):format(key, nn)

    local idx = type(origin_index) == "number" and origin_index or origin.index
    if type(idx) ~= "number" then idx = #screen.tags end
    local tag = awful.tag.add(name, {
        index = idx + 1,
        screen = screen,
        layout = awful.layout.suit.max,
        -- NOT volatile: a volatile focus tag is auto-deleted by awful on the
        -- client's "untagged" signal (before the compositor's close/unmanage
        -- handler can react), and tag.delete then selects the screen's first
        -- tag instead of letting the close path choose the space to land on.
        -- The focus-space module owns the lifecycle via drop_temp_tag.
        volatile = false,
    })
    if not tag then return nil end
    tag.backdrop = "black"

    local origin_id = ""
    local d = desktop_entry_for_tag(origin)
    if d then origin_id = d.id end

    local entry = {
        id = next_id_str(),
        name = name,
        label = app_label(c),
        app_key = key,
        client_id = c.id,
        origin_id = origin_id,
        tag = tag,
        client = c,
    }
    se.focus[#se.focus + 1] = entry
    focus_by_tag[tag] = entry

    changed(true)
    return tag
end

-- ---------------------------------------------------------------------------
-- Public operations
-- ---------------------------------------------------------------------------

function M.add(s)
    s = s or awful.screen.focused()
    if not s then return nil, "no screen" end
    local se = screen_state(s)
    local max = workspaces_max()
    if #se.desktops + #se.focus >= max then
        return nil, ("screen already has %d workspaces"):format(max)
    end
    local n = #se.desktops + 1
    local t = awful.tag.add(("desktop_%02d"):format(n), {
        screen = s,
        layout = default_layout(),
        index = #s.tags + 1,
    })
    if not t then return nil, "failed to create desktop" end
    local entry = { id = next_id_str(), tag = t }
    se.desktops[#se.desktops + 1] = entry
    desktops_by_tag[t] = entry
    changed(true)
    return entry.id
end

local function resolve_id(id)
    for _, se in pairs(state.screens) do
        for _, d in ipairs(se.desktops) do
            if d.id == id and tag_alive(d.tag) then return d, "desktop" end
        end
        for _, f in ipairs(se.focus) do
            if f.id == id and tag_alive(f.tag) then return f, "focus" end
        end
    end
    return nil
end

local function resolve_selected(s)
    s = s or awful.screen.focused()
    if not s then return nil end
    local t = s.selected_tag
    if not (tag_alive(t)) then return nil end
    return resolve_tag(t)
end

resolve_tag = function(t)
    local d = desktop_entry_for_tag(t)
    if d then return d, "desktop" end
    local f = focus_entry_for_tag(t)
    if f then return f, "focus" end
    return nil
end

function M.remove(id)
    local entry, kind
    if id == nil then
        entry, kind = resolve_selected()
        if not entry then return false, "no selected workspace" end
    else
        entry, kind = resolve_id(id)
        if not entry then return false, "no such workspace: " .. tostring(id) end
    end

    if kind == "focus" then
        -- "Remove" a focus space = leave focus mode: the window returns to its
        -- origin exactly like today's leave (property::maximized -> leave()).
        local c = entry.client
        if c and c.valid and c.maximized then
            c.maximized = false
            return true, "left focus space"
        end
        return false, "focus window is gone"
    end

    -- desktop
    local screen = entry.tag.screen
    if not screen then return false, "desktop has no screen" end
    local se = state.screens[connector(screen)]
    if not se or #se.desktops <= 1 then
        return false, "cannot remove the only desktop"
    end
    local idx
    for i, d in ipairs(se.desktops) do
        if d == entry then idx = i break end
    end
    if not idx then return false, "desktop not found" end
    -- The neighbor on the left, or the right when the removed one is first.
    local target = se.desktops[idx == 1 and 2 or idx - 1]
    if not (target and tag_alive(target.tag)) then
        return false, "no neighbor desktop"
    end
    local was_selected = entry.tag.selected

    -- Move the desktop's windows to the neighbor.
    for _, c in ipairs(entry.tag:clients()) do
        if c.valid then c:move_to_tag(target.tag) end
    end

    -- Re-point focus spaces that originated on this desktop.
    for _, f in ipairs(se.focus) do
        if f.origin_id == entry.id and f.client and f.client.valid then
            f.origin_id = target.id
            f.client.focus_space_origin = target.tag
            f.client.focus_space_origin_name = target.tag.name
            f.client.focus_space_origin_index = target.tag.index
        end
    end

    -- View the neighbor with a normal 1->1 slide before the tag goes away.
    if was_selected then target.tag:view_only() end

    entry.tag:delete()
    desktops_by_tag[entry.tag] = nil
    table.remove(se.desktops, idx)
    renumber(se)
    reindex_screen(screen, focus_placement())
    changed(true)
    return true, "removed"
end

function M.view(id)
    local entry = resolve_id(id)
    if not entry then return false, "no such workspace: " .. tostring(id) end
    if not entry.tag.selected then entry.tag:view_only() end
    return true
end

function M.view_desktop(n, s)
    if type(n) ~= "number" then return false, "desktop number required" end
    s = s or awful.screen.focused()
    if not s then return false, "no screen" end
    local se = state.screens[connector(s)]
    local d = se and se.desktops[n]
    if not (d and tag_alive(d.tag)) then
        return false, ("no desktop %d on this screen"):format(n)
    end
    d.tag:view_only()
    return true
end

-- Ordered list of visible tags for a screen: s.tags order (desktops, focus
-- spaces and foreign tags interleaved as the module placed them). Mirrors
-- awful.tag.viewidx's hide filter so foreign hidden tags are skipped too.
local function visible_tags(s)
    local tags = {}
    for _, t in ipairs(s.tags) do
        if not awful.tag.getproperty(t, "hide") then
            tags[#tags + 1] = t
        end
    end
    return tags
end

-- Step one workspace in `dir` (1 = next, -1 = prev). When no slide is running
-- this is a normal view (full duration); while a slide runs the step is queued
-- and replayed by on_slide_end as a chained slide. Hard edges stay no-ops.
local function step(dir, s)
    s = s or awful.screen.focused()
    if not s then return false, "no screen" end
    local tags = visible_tags(s)
    local sel = s.selected_tag
    if not tag_alive(sel) then return false, "no selected workspace" end
    local idx
    for i, t in ipairs(tags) do
        if t == sel then idx = i break end
    end
    if not idx then return false, "selected workspace not in list" end
    if not tag_alive(tags[idx + dir]) then return false end
    if slide_running() then
        if dir ~= step_queue[#step_queue] then
            step_queue = {}
        end
        if #step_queue >= STEP_QUEUE_DEPTH then
            table.remove(step_queue, 1)
        end
        step_queue[#step_queue + 1] = dir
        return true
    end
    if not base_duration then base_duration = slide_last_duration() end
    if base_duration then slide_set_duration(base_duration) end
    tags[idx + dir]:view_only()
    return true
end

local chain_step

-- Pop the next queued step and play it as a chained (faster) slide.
chain_step = function()
    local dir = table.remove(step_queue, 1)
    if not dir then
        if base_duration then slide_set_duration(base_duration) end
        return
    end
    local s = awful.screen.focused()
    if not s then return end
    local tags = visible_tags(s)
    local sel = s.selected_tag
    if not tag_alive(sel) then
        step_queue = {}
        return
    end
    local idx
    for i, t in ipairs(tags) do
        if t == sel then idx = i break end
    end
    local target = idx and tags[idx + dir]
    if not tag_alive(target) then
        -- Skip steps the edge or another view already absorbed.
        return chain_step()
    end
    local chain = CHAIN_FLOOR
    local base = base_duration or slide_last_duration()
    if base then
        chain = math.max(CHAIN_FLOOR, base * CHAIN_FACTOR)
    end
    slide_set_duration(chain)
    target:view_only()
end

local function on_slide_end()
    if #step_queue > 0 then chain_step() end
end

function M.next(s)
    return step(1, s)
end

function M.prev(s)
    return step(-1, s)
end

function M.move_focused(n, s)
    if type(n) ~= "number" then return false, "desktop number required" end
    local c = client.focus
    if not c then return false, "no focused client" end
    local screen = s or c.screen or awful.screen.focused()
    if not screen then return false, "no screen" end
    local se = state.screens[connector(screen)]
    local d = se and se.desktops[n]
    if not (d and tag_alive(d.tag)) then
        return false, ("no desktop %d on this screen"):format(n)
    end
    if c.focus_space_origin then
        -- Leave the focus space first so the client is not moved while its
        -- focus-space state (origin, temp tag) is still attached.
        c.focus_space_unmaximizing = true
        c.maximized = false
    end
    if c.valid then c:move_to_tag(d.tag) end
    return true
end

-- ---------------------------------------------------------------------------
-- Setup
-- ---------------------------------------------------------------------------

local function install_ipc()
    local ok, ipc = pcall(require, "awful.ipc")
    if not (ok and ipc and ipc.register) then return end

    ipc.register("workspace.add", function(screen_arg)
        local s
        if screen_arg then
            local idx = tonumber(screen_arg)
            s = idx and awful.screen[idx] or nil
        end
        local id, err = M.add(s)
        if not id then error(err or "failed to add workspace") end
        return id
    end)

    ipc.register("workspace.remove", function(id)
        local okk, err = M.remove(id ~= "" and id or nil)
        if not okk then error(err or "remove failed") end
        return ""
    end)

    ipc.register("workspace.view", function(id)
        local okk, err = M.view(id)
        if not okk then error(err or "view failed") end
        return ""
    end)

    ipc.register("workspace.view_desktop", function(n, screen_arg)
        local s
        if screen_arg then
            local idx = tonumber(screen_arg)
            s = idx and awful.screen[idx] or nil
        end
        local okk, err = M.view_desktop(tonumber(n), s)
        if not okk then error(err or "view failed") end
        return ""
    end)

    -- For (re)connects: re-announce the full snapshot as an event. The reply
    -- is just OK; Quickshell gets the state from the broadcast.
    ipc.register("workspace.list", function()
        M.broadcast(true)
        return ""
    end)
end

local function create_desktop(se, s, index)
    local t = awful.tag.add(("desktop_%02d"):format(index), {
        screen = s,
        layout = default_layout(),
        index = (#s.tags or 0) + 1,
    })
    if not t then return nil end
    local entry = { id = next_id_str(), tag = t }
    se.desktops[index] = entry
    desktops_by_tag[t] = entry
    return entry
end

local function on_desktop_decoration(s)
    if not s then return end
    local se = screen_state(s)
    local structural = false

    -- Desktops: recreate from persisted order; first start without state (or
    -- after a screen merge) gets one desktop per screen.
    if #se.desktops == 0 then
        if create_desktop(se, s, 1) then structural = true end
    end
    for i, d in ipairs(se.desktops) do
        if not (tag_alive(d.tag)) then
            local t = awful.tag.add(("desktop_%02d"):format(i), {
                screen = s,
                layout = default_layout(),
                index = #s.tags + 1,
            })
            if t then
                d.tag = t
                desktops_by_tag[t] = d
                structural = true
            end
        end
    end

    -- Focus spaces: only materialize when their client survived (hot-reload).
    -- Recreate the tag right after the origin so the C retag (by screen+name)
    -- maps the old temp tag to it and the client is re-attached to the same
    -- space; focus_space.lua adopts it because the origin fields are restored.
    local adopted = {}
    for _, f in ipairs(se.focus) do
        if not (tag_alive(f.tag)) then
            local c = find_client_by_id(f.client_id)
            if c and c.valid and c.maximized and not c.fullscreen then
                local origin
                for _, d in ipairs(se.desktops) do
                    if d.id == f.origin_id then origin = d break end
                end
                if origin and tag_alive(origin.tag) then
                    local t = awful.tag.add(f.name, {
                        screen = s,
                        layout = awful.layout.suit.max,
                        index = origin.tag.index + 1,
                        volatile = true,
                    })
                    if t then
                        t.backdrop = "black"
                        f.tag = t
                        f.client = c
                        focus_by_tag[t] = f
                        adopted[#adopted + 1] = f
                        -- Adopt: restore the origin state so focus_space.lua
                        -- sees a healthy space and never creates a new tag.
                        c.focus_space_tag = t
                        c.focus_space_origin = origin.tag
                        c.focus_space_origin_name = origin.tag.name
                        c.focus_space_origin_index = origin.tag.index
                        c.focus_space_screen = s
                        c.focus_space_tags = { origin.tag }
                        -- The pre-reload decoration snapshot is gone; a default
                        -- one makes leave() re-apply titlebars/border through
                        -- the normal rule/theme path (the C maximize geometry
                        -- restore brings the frame back on unmaximize).
                        c.focus_space_saved = {
                            titlebars_enabled = true,
                            user_border_width = nil,
                            border_inner_enabled = nil,
                            corner_radius_override = nil,
                        }
                        -- Same for the return frame: nothing better survived,
                        -- so fall back to a centered 70 % like a fresh space.
                        if s.workarea then
                            local wa = s.workarea
                            c.focus_space_restore_geometry = {
                                x = wa.x + math.floor(wa.width * 0.15),
                                y = wa.y + math.floor(wa.height * 0.15),
                                width = math.floor(wa.width * 0.7),
                                height = math.floor(wa.height * 0.7),
                            }
                        end
                        pcall(function() c:move_to_tag(t) end)
                    end
                end
            end
        else
            adopted[#adopted + 1] = f
        end
    end
    -- Drop focus spaces that could not be re-attached (client gone or not
    -- maximized): they no longer exist as live spaces. Focus tags are not
    -- volatile (the close/leave path owns their deletion), so sweep any
    -- still-alive empty tag of a dropped space as a safety net.
    if #se.focus ~= #adopted then
        for _, f in ipairs(se.focus) do
            local dropped = true
            for _, a in ipairs(adopted) do
                if a == f then dropped = false break end
            end
            if dropped and tag_alive(f.tag) then
                local ok, count = pcall(function() return #f.tag:clients() end)
                if ok and count == 0 then
                    pcall(function() f.tag:delete() end)
                end
            end
        end
        se.focus = adopted
        structural = true
    end

    -- Every screen needs exactly one selected desktop.
    local sel = s.selected_tag
    if not (tag_alive(sel)) then
        local first = se.desktops[1] and se.desktops[1].tag
        if tag_alive(first) then first:view_only() end
    end

    -- Persist the materialized layout so a later reload sees the same order.
    changed(structural)
end

function M.setup(opts)
    opts = opts or {}
    if type(opts.state_dir) == "string" then
        state_dir = opts.state_dir
    end
    if M._initialized then return M end
    M._initialized = true
    load_state()

    -- The module owns screen removal now: disconnect the fork's per-connector
    -- tag salvage, which would otherwise keep stale saved_tags.
    pcall(function()
        local perms = require("awful.permissions")
        if perms and perms.tag_screen then
            tag.disconnect_signal("request::screen", perms.tag_screen)
        end
    end)

    client.connect_signal("tagged", function() resync() delayed_resync() end)
    client.connect_signal("untagged", function() resync() delayed_resync() end)
    client.connect_signal("unmanage", function() resync() delayed_resync() end)
    tag.connect_signal("property::selected", function() resync() end)
    tag.connect_signal("property::backdrop", function() resync() end)
    tag.connect_signal("request::screen", on_tag_screen_removed)
    screen.connect_signal("removed", on_screen_removed)
    tag.connect_signal("slide_end", on_slide_end)

    screen.connect_signal("request::desktop_decoration", on_desktop_decoration)

    install_ipc()
    return M
end

return M