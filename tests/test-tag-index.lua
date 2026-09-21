-- Test: tag index handling (add/move/delete) stays contiguous and ordered.
--
-- Regression test for the fork's tag index handling. The C tag class exposes a
-- `screen` property which shadows awful's Lua one; `tag.add` seeds the awful
-- properties before applying them, so when `pairs()` applied `index` before
-- `screen`, `set_index` did not see the new tag in `raw_tags()` and the renumber
-- was skipped (the raised error was swallowed by luaA_call_handler). The result
-- was duplicated indexes and a wrong order, e.g. `Z=1 dev=1 chat=2 web=2 ...`.
--
-- Every mutation must leave `screen.tags` as indexes 1..N, unique, in screen
-- order, and every tag whose index changes must emit `property::index` (the
-- taglist listens to it).

local runner = require("_runner")
local awful = require("awful")
local test_client = require("_client")

local function tags_string(s)
    local parts = {}
    for _, t in ipairs(s.tags) do
        parts[#parts + 1] = string.format("%s=%s", t.name, tostring(t.index))
    end
    return table.concat(parts, " ")
end

-- Assert screen.tags indexes are exactly 1..#tags, unique and in order, and
-- that the names match `expected`.
local function check(s, expected, label)
    local tags = s.tags
    assert(
        #tags == #expected,
        string.format("%s: expected %d tags, got %d (%s)", label, #expected, #tags, tags_string(s))
    )
    for i, t in ipairs(tags) do
        assert(
            t.index == i,
            string.format(
                "%s: tag #%d (%s) has index %s, expected %d (%s)",
                label,
                i,
                t.name,
                tostring(t.index),
                i,
                tags_string(s)
            )
        )
        assert(
            t.name == expected[i],
            string.format("%s: tag #%d is %q, expected %q (%s)", label, i, t.name, expected[i], tags_string(s))
        )
    end
end

local function reset(s, names)
    for i = #s.tags, 1, -1 do
        s.tags[i]:delete()
    end
    awful.tag.new(names, s, awful.layout.suit.floating)
end

-- Track `property::index` emissions per tag name.
local emitted = {}
local function watch(t)
    emitted[t] = false
    t:connect_signal("property::index", function()
        emitted[t] = true
    end)
end

local function assert_emitted(label, tags)
    for _, t in ipairs(tags) do
        assert(emitted[t], string.format("%s: %q changed index but did not emit property::index", label, t.name))
    end
end

local function clear_emitted(tags)
    for _, t in ipairs(tags) do
        emitted[t] = false
    end
end

local function by_name(s, name)
    return awful.tag.find_by_name(s, name)
end

local spawned_pid = nil
local vol_temp = nil

local steps = {
    -- 5 tags, baseline.
    function()
        local s = screen.primary
        reset(s, { "dev", "chat", "web", "files", "media" })
        for _, t in ipairs(s.tags) do
            watch(t)
        end
        check(s, { "dev", "chat", "web", "files", "media" }, "baseline")
        io.stderr:write("[tag-index] baseline: " .. tags_string(s) .. "\n")
        return true
    end,

    -- add at index 1
    function()
        local s = screen.primary
        clear_emitted(s.tags)
        awful.tag.add("Z", { index = 1, volatile = true })
        check(s, { "Z", "dev", "chat", "web", "files", "media" }, "add index=1")
        -- dev..media shifted, Z kept index 1.
        assert_emitted(
            "add index=1",
            { by_name(s, "dev"), by_name(s, "chat"), by_name(s, "web"), by_name(s, "files"), by_name(s, "media") }
        )
        io.stderr:write("[tag-index] add index=1: " .. tags_string(s) .. "\n")
        return true
    end,

    -- add at index 2
    function()
        local s = screen.primary
        clear_emitted(s.tags)
        awful.tag.add("Y", { index = 2, volatile = true })
        check(s, { "Z", "Y", "dev", "chat", "web", "files", "media" }, "add index=2")
        assert_emitted(
            "add index=2",
            { by_name(s, "dev"), by_name(s, "chat"), by_name(s, "web"), by_name(s, "files"), by_name(s, "media") }
        )
        io.stderr:write("[tag-index] add index=2: " .. tags_string(s) .. "\n")
        return true
    end,

    -- add at index 3
    function()
        local s = screen.primary
        clear_emitted(s.tags)
        awful.tag.add("X", { index = 3, volatile = true })
        check(s, { "Z", "Y", "X", "dev", "chat", "web", "files", "media" }, "add index=3")
        io.stderr:write("[tag-index] add index=3: " .. tags_string(s) .. "\n")
        return true
    end,

    -- add at the end (no explicit index)
    function()
        local s = screen.primary
        clear_emitted(s.tags)
        awful.tag.add("W", { volatile = true })
        check(s, { "Z", "Y", "X", "dev", "chat", "web", "files", "media", "W" }, "add at end")
        io.stderr:write("[tag-index] add at end: " .. tags_string(s) .. "\n")
        return true
    end,

    -- move with t.index: backwards (W to 1) then forwards (W to 4)
    function()
        local s = screen.primary
        clear_emitted(s.tags)
        by_name(s, "W").index = 1
        check(s, { "W", "Z", "Y", "X", "dev", "chat", "web", "files", "media" }, "move W -> 1")
        clear_emitted(s.tags)
        by_name(s, "W").index = 4
        check(s, { "Z", "Y", "X", "W", "dev", "chat", "web", "files", "media" }, "move W -> 4")
        io.stderr:write("[tag-index] move W 1 then 4: " .. tags_string(s) .. "\n")
        return true
    end,

    -- delete a tag in the middle, gap must close
    function()
        local s = screen.primary
        local moved = {
            by_name(s, "dev"),
            by_name(s, "chat"),
            by_name(s, "web"),
            by_name(s, "files"),
            by_name(s, "media"),
        }
        clear_emitted(s.tags)
        by_name(s, "X"):delete()
        check(s, { "Z", "Y", "W", "dev", "chat", "web", "files", "media" }, "delete middle")
        assert_emitted("delete middle", moved)
        io.stderr:write("[tag-index] delete middle: " .. tags_string(s) .. "\n")
        return true
    end,

    -- volatile tag dies when its last client is untagged
    function()
        if not test_client.is_available() then
            io.stderr:write("[tag-index] no terminal; skipping volatile-lifecycle step\n")
            return true
        end
        spawned_pid = test_client("tagindex")
        return true
    end,
    function()
        if spawned_pid and #client.get() == 0 then
            return nil
        end
        if vol_temp then
            return nil
        end
        local c = client.get()[1]
        if not c then
            return nil
        end
        vol_temp = awful.tag.add("temp", { index = 2, volatile = true, layout = awful.layout.suit.floating })
        c:tags({ vol_temp })
        return #vol_temp:clients() == 1 or nil
    end,
    function()
        if not vol_temp then
            return true
        end
        local s = screen.primary
        local c = vol_temp:clients()[1]
        if not c then
            return nil
        end
        -- Move the client away: the volatile tag must auto-delete and close the gap.
        c:tags({ by_name(s, "Z") })
        if vol_temp.activated then
            return nil
        end
        check(s, { "Z", "Y", "W", "dev", "chat", "web", "files", "media" }, "volatile died")
        io.stderr:write("[tag-index] volatile lifecycle: " .. tags_string(s) .. "\n")
        return true
    end,

    -- Deterministic reproduction of the underlying failure mode: a tag whose
    -- screen is applied after its index (which is what the unordered pairs()
    -- loop did) must still be inserted and the following tags renumbered.
    function()
        local s = screen.primary
        local nt = tag({ name = "Q" })
        nt._private.awful_tag_properties = { screen = s, index = 1 }
        nt.activated = true
        nt.screen = nil -- C screen unset, as when `index` ran first
        nt.index = 1 -- must not silently fail on rm_index == nil
        nt.screen = s
        check(s, { "Q", "Z", "Y", "W", "dev", "chat", "web", "files", "media" }, "index before screen")
        io.stderr:write("[tag-index] deterministic index-before-screen: " .. tags_string(s) .. "\n")
        return true
    end,
}

runner.run_steps(steps, { kill_clients = true, wait_per_step = 10 })
