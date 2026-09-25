-- P0.3 GLES2 smoke test config: 3 tags, no autostart, no wibar.
local awful = require("awful")
require("awful.autofocus")
awful.layout.layouts = { awful.layout.suit.floating, awful.layout.suit.tile }
awesome.connect_signal("debug::error", function(err)
    io.stderr:write("ERROR: " .. tostring(err) .. "\n")
end)
for s in screen do
    awful.tag({ "1", "2", "3" }, s, awful.layout.layouts[1])
end
package.loaded["core.autostart"] = {}
