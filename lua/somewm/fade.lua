---------------------------------------------------------------------------
--- Opacity-correct fades for windows and layer-shell panels.
--
-- Tweens `obj.opacity` (a client or a layer_surface) through
-- `awesome.start_animation`. A fade always ends at the object's target
-- opacity -- the value rules or config set -- never at a hardcoded 1.0.
-- Decorations (border, shadow, backdrop blur) track the buffer opacity
-- in C, so this module only ever tweens the one property.
--
-- Fading out a window that is being destroyed is out of scope (keeping
-- its last buffer alive is a separate task); `fade.fade_out` is for hide or
-- minimize flows and calls `opts.done` at zero.
--
-- Nothing here runs on its own; wire it from rc.lua, e.g.
--     local fade = require("somewm.fade")
--     client.connect_signal("request::manage", function(c)
--         if should_fade(c) then fade.fade_in(c) end
--     end)
--
-- @module somewm.fade
---------------------------------------------------------------------------

local fade = {}

--- Default fade duration in seconds.
-- @tfield number duration
fade.duration = 0.25

--- Default easing passed to `awesome.start_animation`
-- (`linear`, `ease-out-cubic`, `ease-in-out-cubic`).
-- @tfield string easing
fade.easing = "ease-out-cubic"

--- Master switch. When false, fades snap to the target instantly.
-- @tfield boolean enabled
fade.enabled = true

-- In-flight fades, keyed weakly by object: { handle, target, restore }.
-- `target` is where the running tween ends; `restore` is the opacity a
-- later `fade.fade_in` returns to (the pre-fade value for `fade.fade_out`).
local active = setmetatable({}, { __mode = "k" })

local function effective(obj)
    local ok, value = pcall(function() return obj.opacity end)
    if ok and type(value) == "number" then
        return value
    end
    return 1.0
end

local function alive(obj)
    if obj == nil then
        return false
    end
    local ok, valid = pcall(function() return obj.valid end)
    if ok then
        return valid ~= false
    end
    -- Layer surfaces expose no `valid`; any readable property means alive.
    return select(1, pcall(function() return obj.opacity end))
end

local function cancel_active(obj)
    local st = active[obj]
    if st and st.handle then
        pcall(function() st.handle:cancel() end)
    end
    return st
end

local function tween(obj, from, to, opts, restore)
    opts = opts or {}
    local duration = opts.duration or fade.duration
    local easing = opts.easing or fade.easing
    local done_cb = opts.done

    cancel_active(obj)

    if not fade.enabled or duration <= 0 then
        if alive(obj) then
            obj.opacity = to
        end
        active[obj] = { handle = nil, target = to, restore = restore or to }
        if done_cb then
            done_cb()
        end
        return nil
    end

    if alive(obj) then
        obj.opacity = from
    end
    local handle = awesome.start_animation(duration, easing,
        function(t)
            if not alive(obj) then
                return
            end
            obj.opacity = from + (to - from) * t
        end,
        function()
            active[obj] = nil
            if alive(obj) then
                obj.opacity = to
            end
            active[obj] = { handle = nil, target = to, restore = restore or to }
            if done_cb then
                done_cb()
            end
        end)
    active[obj] = { handle = handle, target = to, restore = restore or to }
    return handle
end

--- Tween an object to an explicit target opacity.
-- @tparam table obj A client or layer_surface.
-- @tparam number target End opacity 0.0..1.0.
-- @tparam[opt] table opts `duration`, `easing`, `from` (default: current),
--   `done` callback.
-- @treturn handle|nil The animation handle, or nil when snapped/cancelled.
function fade.to(obj, target, opts)
    opts = opts or {}
    local from = opts.from
    if from == nil then
        from = effective(obj)
    end
    return tween(obj, from, target, opts, target)
end

--- Fade an object in, ending at its target opacity (the value rules or
-- config set, read before the first tick).
-- @tparam table obj A client or layer_surface.
-- @tparam[opt] table opts `duration`, `easing`, `from` (default 0),
--   `done` callback.
-- @treturn handle|nil The animation handle, or nil when snapped/cancelled.
function fade.fade_in(obj, opts)
    opts = opts or {}
    local st = active[obj]
    local target = (st and st.restore) or effective(obj)
    local from = opts.from
    if from == nil then
        from = 0
    end
    return tween(obj, from, target, opts, target)
end

--- Fade an object out to zero, then call `opts.done` (hide/minimize flows).
-- Not for windows being destroyed; see the module header.
-- @tparam table obj A client or layer_surface.
-- @tparam[opt] table opts `duration`, `easing`, `done` callback,
--   `restore` (opacity a later `fade.fade_in` returns to; default: pre-fade).
-- @treturn handle|nil The animation handle, or nil when snapped/cancelled.
function fade.fade_out(obj, opts)
    opts = opts or {}
    local st = active[obj]
    local restore = opts.restore or (st and st.restore) or effective(obj)
    return tween(obj, effective(obj), 0, opts, restore)
end

--- Cancel any in-flight fade for an object, leaving the current opacity.
-- @tparam table obj A client or layer_surface.
function fade.cancel(obj)
    cancel_active(obj)
    active[obj] = nil
end

return fade
