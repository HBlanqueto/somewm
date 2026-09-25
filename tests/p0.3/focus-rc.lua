-- P0.3 focus-space scenario: loads the real libs/focus_space against the fork.
local awful = require("awful")
require("awful.autofocus")
awful.layout.layouts = { awful.layout.suit.floating, awful.layout.suit.tile }
-- P0.11: resolve the personal config from the isolated config dir when the
-- harness exports SOMEWM_TEST_CONFIG_DIR; fall back to the live config path.
local cfg_dir = os.getenv("SOMEWM_TEST_CONFIG_DIR") or "/home/humbe/.config/somewm"
package.path = cfg_dir .. "/?.lua;" .. cfg_dir .. "/?/init.lua;" .. package.path
awesome.connect_signal("debug::error", function(err)
    io.stderr:write("ERROR: " .. tostring(err) .. "\n")
end)
for s in screen do
    awful.tag({ "1", "2", "3" }, s, awful.layout.layouts[1])
end
package.loaded["core.autostart"] = {}
require("libs.focus_space")
