-------------------------
-- Roundy awesome theme --
-------------------------
-- A simple dark theme based on the default SomeWM theme, with rounded
-- corners (radius 10) enabled for every object and a rounded drop shadow
-- that follows each window's corner shape. See docs/features_test.md
-- sections 1.3 (rounded corners) and 2.2 (shadows) for the options.

local theme_assets = require("beautiful.theme_assets")
local xresources = require("beautiful.xresources")
local rnotification = require("ruled.notification")
local dpi = xresources.apply_dpi

local gfs = require("gears.filesystem")
local themes_path = gfs.get_themes_dir()

local theme = {}

theme.font = "sans 8"

-- Dark palette (a few color changes from the default theme)
theme.bg_normal = "#1c1c24"
theme.bg_focus = "#4a5a8a"
theme.bg_urgent = "#d94b4b"
theme.bg_minimize = "#2a2a34"
theme.bg_systray = theme.bg_normal

theme.fg_normal = "#c0c0cc"
theme.fg_focus = "#ffffff"
theme.fg_urgent = "#ffffff"
theme.fg_minimize = "#dddddd"

theme.useless_gap = dpi(0)
theme.border_width = dpi(1)
theme.border_color_normal = "#14141a"
theme.border_color_active = "#6d7fd6"
theme.border_color_marked = "#d94b4b"

-- ---------------------------------------------------------------------------
-- Rounded corners (docs/features_test.md section 1.3)
-- Enabled for every object: client windows and drawins/wiboxes/the wibar.
-- Radius 10 on all four corners.
-- ---------------------------------------------------------------------------
theme.corner_enabled = true
theme.corner_radius = 10
theme.corner_color = "#14141a"          -- parsed; cut corners are true alpha
theme.corner_drawin_enabled = true
theme.corner_drawin_radius = 10
theme.corner_drawin_color = "#14141a"

-- ---------------------------------------------------------------------------
-- Shadows (docs/features_test.md section 2.2)
-- Compositor drop shadows that follow the window's rounded corners
-- (shadow_follow_corners). Radius 10 falloff, a slight downward offset.
-- ---------------------------------------------------------------------------
theme.shadow_enabled = true
theme.shadow_radius = 12
theme.shadow_offset_x = 0
theme.shadow_offset_y = 4
theme.shadow_spread = 0
theme.shadow_corner_radius = 0          -- unused while follow_corners is true
theme.shadow_follow_corners = true
theme.shadow_opacity = 0.55
theme.shadow_color = "#000000"

theme.shadow_drawin_enabled = true
theme.shadow_drawin_radius = 12
theme.shadow_drawin_follow_corners = true
theme.shadow_drawin_color = "#000000"

-- ---------------------------------------------------------------------------
-- Inner hairline (docs/features_test.md section 3.3)
-- A thin decorative line just inside the window edge.
-- ---------------------------------------------------------------------------
theme.border_inner_enabled = true
theme.border_inner_width = 1
theme.border_inner_color = "rgba(255, 255, 255, 0.15)"
theme.border_inner_drawin_enabled = true

-- There are other variable sets
-- overriding the default one when
-- defined, the sets are:
-- taglist_[bg|fg]_[focus|urgent|occupied|empty|volatile]
-- tasklist_[bg|fg]_[focus|urgent]
-- titlebar_[bg|fg]_[normal|focus]
-- tooltip_[font|opacity|fg_color|bg_color|border_width|border_color]
-- prompt_[fg|bg|fg_cursor|bg_cursor|font]
-- hotkeys_[bg|fg|border_width|border_color|shape|opacity|modifiers_fg|label_bg|label_fg|group_margin|font|description_font]
-- Example:
--theme.taglist_bg_focus = "#ff0000"

-- Generate taglist squares:
local taglist_square_size = dpi(4)
theme.taglist_squares_sel = theme_assets.taglist_squares_sel(taglist_square_size, theme.fg_normal)
theme.taglist_squares_unsel = theme_assets.taglist_squares_unsel(taglist_square_size, theme.fg_normal)

-- Variables set for theming notifications:
-- notification_font
-- notification_[bg|fg]
-- notification_[width|height|margin]
-- notification_[border_color|border_width|shape|opacity]

-- Variables set for theming the menu:
-- menu_[bg|fg]_[normal|focus]
-- menu_[border_color|border_width]
theme.menu_submenu_icon = themes_path .. "roundy/submenu.png"
theme.menu_height = dpi(15)
theme.menu_width = dpi(100)

-- You can add as many variables as
-- you wish and access them by using
-- beautiful.variable in your rc.lua
--theme.bg_widget = "#cc0000"

-- Define the image to load
theme.titlebar_close_button_normal = themes_path .. "roundy/titlebar/close_normal.png"
theme.titlebar_close_button_focus = themes_path .. "roundy/titlebar/close_focus.png"

theme.titlebar_minimize_button_normal = themes_path .. "roundy/titlebar/minimize_normal.png"
theme.titlebar_minimize_button_focus = themes_path .. "roundy/titlebar/minimize_focus.png"

theme.titlebar_ontop_button_normal_inactive = themes_path .. "roundy/titlebar/ontop_normal_inactive.png"
theme.titlebar_ontop_button_focus_inactive = themes_path .. "roundy/titlebar/ontop_focus_inactive.png"
theme.titlebar_ontop_button_normal_active = themes_path .. "roundy/titlebar/ontop_normal_active.png"
theme.titlebar_ontop_button_focus_active = themes_path .. "roundy/titlebar/ontop_focus_active.png"

theme.titlebar_sticky_button_normal_inactive = themes_path .. "roundy/titlebar/sticky_normal_inactive.png"
theme.titlebar_sticky_button_focus_inactive = themes_path .. "roundy/titlebar/sticky_focus_inactive.png"
theme.titlebar_sticky_button_normal_active = themes_path .. "roundy/titlebar/sticky_normal_active.png"
theme.titlebar_sticky_button_focus_active = themes_path .. "roundy/titlebar/sticky_focus_active.png"

theme.titlebar_floating_button_normal_inactive = themes_path .. "roundy/titlebar/floating_normal_inactive.png"
theme.titlebar_floating_button_focus_inactive = themes_path .. "roundy/titlebar/floating_focus_inactive.png"
theme.titlebar_floating_button_normal_active = themes_path .. "roundy/titlebar/floating_normal_active.png"
theme.titlebar_floating_button_focus_active = themes_path .. "roundy/titlebar/floating_focus_active.png"

theme.titlebar_maximized_button_normal_inactive = themes_path .. "roundy/titlebar/maximized_normal_inactive.png"
theme.titlebar_maximized_button_focus_inactive = themes_path .. "roundy/titlebar/maximized_focus_inactive.png"
theme.titlebar_maximized_button_normal_active = themes_path .. "roundy/titlebar/maximized_normal_active.png"
theme.titlebar_maximized_button_focus_active = themes_path .. "roundy/titlebar/maximized_focus_active.png"

-- Wallpaper: a flat solid colour matching the wibar (bg_normal), with the
-- somewm "S" logo drawn on top, recolored to the theme's blue accent.
-- Setting wallpaper_colors (instead of wallpaper) makes somewmrc.lua's
-- request::wallpaper handler draw the logo; two identical stops render the
-- gradient as a solid colour.
theme.wallpaper_colors = { theme.bg_normal, theme.bg_normal }
theme.wallpaper_logo_color = "#6d7fd6"

-- You can use your own layout icons like this:
theme.layout_fairh = themes_path .. "roundy/layouts/fairhw.png"
theme.layout_fairv = themes_path .. "roundy/layouts/fairvw.png"
theme.layout_floating = themes_path .. "roundy/layouts/floatingw.png"
theme.layout_magnifier = themes_path .. "roundy/layouts/magnifierw.png"
theme.layout_max = themes_path .. "roundy/layouts/maxw.png"
theme.layout_fullscreen = themes_path .. "roundy/layouts/fullscreenw.png"
theme.layout_tilebottom = themes_path .. "roundy/layouts/tilebottomw.png"
theme.layout_tileleft = themes_path .. "roundy/layouts/tileleftw.png"
theme.layout_tile = themes_path .. "roundy/layouts/tilew.png"
theme.layout_tiletop = themes_path .. "roundy/layouts/tiletopw.png"
theme.layout_spiral = themes_path .. "roundy/layouts/spiralw.png"
theme.layout_dwindle = themes_path .. "roundy/layouts/dwindlew.png"
theme.layout_cornernw = themes_path .. "roundy/layouts/cornernww.png"
theme.layout_cornerne = themes_path .. "roundy/layouts/cornernew.png"
theme.layout_cornersw = themes_path .. "roundy/layouts/cornersww.png"
theme.layout_cornerse = themes_path .. "roundy/layouts/cornersew.png"

-- Generate Awesome icon:
theme.awesome_icon = theme_assets.awesome_icon(theme.menu_height, theme.bg_focus, theme.fg_focus)

-- Define the icon theme for application icons. If not set then the icons
-- from /usr/share/icons and /usr/share/icons/hicolor will be used.
theme.icon_theme = nil

-- Set different colors for urgent notifications.
rnotification.connect_signal("request::rules", function()
  rnotification.append_rule({
    rule = { urgency = "critical" },
    properties = { bg = "#d94b4b", fg = "#ffffff" },
  })
end)

return theme

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
