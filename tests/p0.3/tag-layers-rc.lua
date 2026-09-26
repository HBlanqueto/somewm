-- P0.3 tag.layers test config: 3 tags, tag "2" claims namespace "tag-layer-test".
-- No autostart, no wibar. The surface appears/disappears/slides with tag 2.
local awful = require("awful")
require("awful.autofocus")
awful.layout.layouts = { awful.layout.suit.floating, awful.layout.suit.tile }
awesome.connect_signal("debug::error", function(err)
    io.stderr:write("ERROR: " .. tostring(err) .. "\n")
end)
for s in screen do
    awful.tag({ "1", "2", "3" }, s, awful.layout.layouts[1])
end
for _, t in ipairs(screen[1].tags) do
    if t.name == "2" then
        t.layers = { "tag-layer-test" }
    end
end
package.loaded["core.autostart"] = {}