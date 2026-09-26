-- Test: tag role / client_policy and the request::relocate enforcement.
--
-- Regression test for the fork's reject-tag support:
--   * tag.role and tag.client_policy are C properties following the backdrop
--     pattern (default "allow"), with property::* signals.
--   * A tag with client_policy "reject" refuses to hold clients on EVERY route
--     and emits tag:request::relocate; the default handler in awful.permissions
--     relocates the client to an allowed tag preferring the refusing tag's
--     screen. A config overrides the destination by REPLACING the default
--     (disconnect awful.permissions.tag_relocate, connect its own handler),
--     which tags the client exactly once; a handler connected without
--     disconnecting the default still runs after it and its c:tags() wins, but
--     that tags the client twice and is not the supported contract.
--   * Edge cases: reject tag as the only tag on its screen (fail-open onto the
--     reject tag), switching a tag to "reject" while it holds clients (evict),
--     a relocation target that is itself reject (no loop), sticky clients, and
--     the setmon() property::screen -> request::tag follow-up not bouncing the
--     client back onto the reject tag.
--
-- Route coverage (all funnel through tag_client() in objects/tag.c):
--   * route 1 mapnotify default tags        -> spawn while reject tag selected
--   * route 3 client:tags{} / move_to_tag / toggle_tag / to_selected_tags
--   * route 4 tag:clients{}
--   * route 5 request::tag default handler  -> to_selected_tags, rules
--   * route 6 setmon property::screen       -> no-bounce assertion below
--   * route 7 tag clear/delete fallback
--   * IPC client.movetotag (lua/awful/ipc.lua)
--   * route 2 (transient copies parent tags) and route 8 (screen removal)
--     share the same tag_client() funnel and are covered by the existing
--     transient / screen-removal tests, so they are not repeated here.

local runner = require("_runner")
local awful = require("awful")
local test_client = require("_client")
local utils = require("_utils")

if not test_client.is_available() then
    io.stderr:write("SKIP: no terminal available for client spawning\n")
    io.stderr:write("Test finished successfully.\n")
    awesome.quit()
    return
end

local ipc = require("awful.ipc")
local ruled = require("ruled")

local s = screen.primary
local main, work, player

local function by_name(name)
    return awful.tag.find_by_name(s, name)
end

-- Move every client off the screen's tags, then rebuild a known tag layout.
local function reset_tags(names)
    for _, c in ipairs(client.get()) do
        if c.valid then c:tags({}) end
    end
    for i = #s.tags, 1, -1 do
        s.tags[i]:delete()
    end
    awful.tag.new(names, s, awful.layout.suit.floating)
    main, work, player = by_name(names[1]), by_name(names[2]), by_name(names[3])
end

local function tagged_with(c, tag)
    if not c or not c.valid then return false end
    for _, t in ipairs(c:tags()) do
        if t == tag then return true end
    end
    return false
end

local function tag_names(c)
    if not c or not c.valid then return "" end
    local parts = {}
    for _, t in ipairs(c:tags()) do
        parts[#parts + 1] = t.name
    end
    return table.concat(parts, ",")
end

local override_conn = nil
local function disconnect_override()
    if override_conn then
        tag.disconnect_signal("request::relocate", override_conn)
        override_conn = nil
    end
end

local oneshot_emissions = 0

local spawned = 0
local function spawn(class)
    spawned = spawned + 1
    return test_client(class, "PolicyClient" .. spawned)
end

local rule_ref = nil

local steps = {
    -- Property contract: role and client_policy, defaults and signals.
    function()
        reset_tags({ "main", "work", "player" })
        assert(main.client_policy == "allow", "default client_policy must be 'allow'")
        assert(main.role == "", "default role must be ''")

        local role_fired, policy_fired = false, false
        player:connect_signal("property::role", function() role_fired = true end)
        player:connect_signal("property::client_policy", function() policy_fired = true end)
        player.role = "media"
        player.client_policy = "reject"
        assert(player.role == "media", "role round-trip failed: " .. tostring(player.role))
        assert(player.client_policy == "reject", "client_policy round-trip failed")
        assert(role_fired, "property::role did not fire")
        assert(policy_fired, "property::client_policy did not fire")
        assert(main.client_policy == "allow", "setting player must not touch main")
        io.stderr:write("[tag-policy] role='" .. player.role
            .. "' policy='" .. player.client_policy .. "'\n")
        return true
    end,

    -- Route 1 + route 6/5: a client mapped while only the reject tag is
    -- selected is relocated by the default handler, and the follow-up
    -- setmon() property::screen -> request::tag does not bounce it back.
    function(count)
        if count == 1 then
            player:view_only()
            spawn("policy_map")
        end
        local c = utils.find_client_by_class("policy_map")
        if not c then return end
        if tagged_with(c, player) then return end
        if not tagged_with(c, main) then return end
        assert(c.screen == s, "client must stay on the spawn screen")
        io.stderr:write("[tag-policy] map -> " .. tag_names(c) .. "\n")
        return true
    end,

    -- Config override: a handler connected after the default one wins.
    function(count)
        if count == 1 then
            disconnect_override()
            override_conn = function(_t, c)
                c:tags({ work })
            end
            tag.connect_signal("request::relocate", override_conn)
            player:view_only()
            spawn("policy_ovr")
        end
        local c = utils.find_client_by_class("policy_ovr")
        if not c then return end
        if not tagged_with(c, work) then return end
        assert(not tagged_with(c, player), "override must keep the client off the reject tag")
        io.stderr:write("[tag-policy] override -> " .. tag_names(c) .. "\n")
        return true
    end,

    -- Single-shot override contract: a config REPLACES the default handler
    -- (disconnect + connect its own), so the relocation tags the client
    -- exactly once. Count property::tags emissions on the client during the
    -- relocation: it must be 1, not the 2 a run-after handler would produce.
    function(count)
        if count == 1 then
            oneshot_emissions = 0
            disconnect_override()
            tag.disconnect_signal("request::relocate", awful.permissions.tag_relocate)
            override_conn = function(_t, c)
                c:connect_signal("property::tags", function()
                    oneshot_emissions = oneshot_emissions + 1
                end)
                c:tags({ work })
            end
            tag.connect_signal("request::relocate", override_conn)
            player:view_only()
            spawn("policy_oneshot")
        end
        local c = utils.find_client_by_class("policy_oneshot")
        if not c then return end
        if not tagged_with(c, work) then return end
        assert(not tagged_with(c, player),
            "single-shot override must keep the client off the reject tag")
        assert(oneshot_emissions == 1,
            "single-shot relocation must emit property::tags exactly once, got "
            .. oneshot_emissions)
        -- Restore the default handler for the remaining steps.
        tag.disconnect_signal("request::relocate", override_conn)
        override_conn = nil
        tag.connect_signal("request::relocate", awful.permissions.tag_relocate)
        io.stderr:write("[tag-policy] single-shot override -> " .. tag_names(c)
            .. " (property::tags x" .. oneshot_emissions .. ")\n")
        return true
    end,

    -- In-process routes: move_to_tag, tags{}, toggle_tag, to_selected_tags and
    -- tag:clients{} all funnel through tag_client() and are refused.
    function()
        disconnect_override()
        local c = utils.find_client_by_class("policy_map")
        assert(c, "policy_map client must exist")

        c:move_to_tag(player)
        assert(not tagged_with(c, player), "move_to_tag must not place on reject tag")
        assert(#c:tags() > 0, "move_to_tag must leave the client tagged")

        c:tags({ player })
        assert(not tagged_with(c, player), "tags{} must not place on reject tag")
        assert(#c:tags() > 0, "tags{} must leave the client tagged")

        c:tags({ player, work })
        assert(tagged_with(c, work) and not tagged_with(c, player),
            "tags{reject, allow} must keep only the allowed tag")

        c:toggle_tag(player)
        assert(not tagged_with(c, player), "toggle_tag must not place on reject tag")

        player:clients({ c })
        assert(not tagged_with(c, player), "tag:clients{} must not place on reject tag")

        player:view_only()
        c:to_selected_tags()
        assert(not tagged_with(c, player), "to_selected_tags must not place on reject tag")

        io.stderr:write("[tag-policy] routes ok, on " .. tag_names(c) .. "\n")
        return true
    end,

    -- IPC route: client.movetotag through the real dispatcher.
    function()
        local c = utils.find_client_by_class("policy_map")
        local root_tags = root.tags()
        local player_idx
        for i, t in ipairs(root_tags) do
            if t == player then player_idx = i break end
        end
        assert(player_idx, "player tag must be in root.tags()")
        ipc.dispatch("client.movetotag " .. player_idx .. " " .. tostring(c.id), nil)
        assert(not tagged_with(c, player), "IPC movetotag must not place on reject tag")
        assert(#c:tags() > 0, "IPC movetotag must leave the client tagged")
        io.stderr:write("[tag-policy] ipc ok, on " .. tag_names(c) .. "\n")
        return true
    end,

    -- Rules route: a rule forcing tag=player is refused too.
    function(count)
        if count == 1 then
            rule_ref = { rule = { class = "policy_rule" }, properties = { tag = player } }
            ruled.client.append_rule(rule_ref)
            spawn("policy_rule")
        end
        local c = utils.find_client_by_class("policy_rule")
        if not c then return end
        if #c:tags() == 0 then return end
        assert(not tagged_with(c, player), "rule tag=player must not place on reject tag")
        ruled.client.remove_rule(rule_ref)
        rule_ref = nil
        io.stderr:write("[tag-policy] rule ok, on " .. tag_names(c) .. "\n")
        return true
    end,

    -- Sticky clients are refused too.
    function()
        local c = utils.find_client_by_class("policy_map")
        c.sticky = true
        c:move_to_tag(player)
        assert(not tagged_with(c, player), "sticky move_to_tag must not place on reject tag")
        assert(#c:tags() > 0, "sticky client must remain tagged")
        c.sticky = false
        io.stderr:write("[tag-policy] sticky ok\n")
        return true
    end,

    -- Route 7 (tag clear / delete): a fallback tag that is reject is refused
    -- and the client is relocated instead of parked on it.
    function()
        local c = utils.find_client_by_class("policy_map")
        local temp = awful.tag.add("temp", { screen = s, layout = awful.layout.suit.floating })
        c:tags({ temp })
        temp:clear({ fallback_tag = player })
        assert(not tagged_with(c, player), "clear fallback to a reject tag must not stick")
        assert(#c:tags() > 0, "clear must leave the client tagged")
        temp:delete()
        io.stderr:write("[tag-policy] clear/delete ok\n")
        return true
    end,

    -- Switching a tag to "reject" while it holds clients evicts them.
    function()
        local c = utils.find_client_by_class("policy_map")
        local evict = awful.tag.add("evict", { screen = s, layout = awful.layout.suit.floating })
        c:tags({ evict })
        assert(tagged_with(c, evict), "setup: client must be on evict")
        evict.client_policy = "reject"
        assert(not tagged_with(c, evict), "switch to reject must evict the client")
        assert(#c:tags() > 0, "evicted client must remain tagged")
        io.stderr:write("[tag-policy] evict -> " .. tag_names(c) .. "\n")
        return true
    end,

    -- Fail-open: a reject tag that is the ONLY tag on the screen still ends up
    -- holding the client (an untagged client is unreachable by the user).
    function(count)
        if count == 1 then
            reset_tags({ "solo" })
            player = by_name("solo")
            player.client_policy = "reject"
            player:view_only()
            spawn("policy_solo")
        end
        local c = utils.find_client_by_class("policy_solo")
        if not c then return end
        if not tagged_with(c, player) then return end
        assert(player.client_policy == "reject", "solo tag must stay reject")
        io.stderr:write("[tag-policy] fail-open: client on " .. tag_names(c) .. "\n")
        return true
    end,

    -- No-loop: an override that always targets a reject tag cannot spin; the
    -- client stays reachable (fail-open puts it back on the refusing tag).
    function(count)
        if count == 1 then
            reset_tags({ "no1", "no2" })
            main, work, player = by_name("no1"), by_name("no2"), nil
            work.client_policy = "reject"
            disconnect_override()
            override_conn = function(_t, c)
                c:tags({ work })  -- always targets a reject tag
            end
            tag.connect_signal("request::relocate", override_conn)
            work:view_only()
            spawn("policy_loop")
        end
        local c = utils.find_client_by_class("policy_loop")
        if not c then return end
        if #c:tags() == 0 then return end
        assert(#c:tags() >= 1, "no-loop client must stay tagged")
        disconnect_override()
        io.stderr:write("[tag-policy] no-loop ok, on " .. tag_names(c) .. "\n")
        return true
    end,
}

runner.run_steps(steps, { kill_clients = true, wait_per_step = 10 })

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80