# SomeWM Deviations from AwesomeWM

This document tracks all known differences between somewm and AwesomeWM. These exist primarily due to fundamental differences between X11 and Wayland protocols.

## Architectural Differences (Wayland vs X11)

| Feature | AwesomeWM (X11) | SomeWM (Wayland) | Reason |
|---------|-----------------|------------------|--------|
| Systray | X11 `_NET_SYSTEMTRAY` embed | StatusNotifierItem D-Bus (SNI) | X11 tray protocol doesn't exist on Wayland |
| Titlebar borders | Outside frame (X server draws) | Inset by `border_width` | Scene graph positioning differs |
| Window visibility | `xcb_map_window()` shows immediately | Content must exist before showing | Prevents smearing artifacts |
| WM restart | `awesome.restart()` re-execs the process | In-process Lua hot-reload (clients survive) | Wayland compositor can't re-exec; tears down and rebuilds Lua VM instead |
| GTK theme detection | Creates GTK widgets, queries `GtkStyleContext` | Parses `gtk-3.0/settings.ini` and `gtk-4.0/settings.ini` | Creating GTK windows inside a compositor is unsafe |
| Xresources | Queries `xrdb` server | Parses `~/.Xresources` file directly | No `xrdb` server on Wayland |
| Wibox shape surfaces | 1-bit (`cairo.Format.A1`) | Full ARGB32 with anti-aliasing | Enables anti-aliased rounded corners and HiDPI scaling |
| Config/cache paths | `~/.config/awesome/`, `~/.cache/awesome/` | `~/.config/somewm/`, `~/.cache/somewm/` | Rebranded |

### Detailed Explanations

**Systray (SNI vs X11 embed)**
- AwesomeWM uses X11's `_NET_SYSTEMTRAY` protocol to embed tray icon windows
- SomeWM uses the modern StatusNotifierItem D-Bus protocol
- Most apps (NetworkManager, Discord, Bluetooth) support SNI already
- Legacy XEmbed-only apps won't show tray icons

**Titlebar Border Positioning**
- In X11, borders are drawn OUTSIDE the window frame by the X server
- In Wayland, somewm draws borders as scene rects around the geometry: the scene tree origin is the outer border corner and the client footprint is `geometry` plus `border_width` on each side
- `c->geometry` excludes the border, matching AwesomeWM
- Titlebars are inside the geometry and must start INSIDE the border area, hence `border_width` inset
- See `titlebar_get_area()` in `objects/client.c`

**WM Restart**
- AwesomeWM re-execs itself via `execvp()`, restarting the entire process
- SomeWM performs in-process Lua hot-reload: tears down the Lua VM, rebuilds it from `rc.lua`, and reattaches existing clients
- wlroots, the scene graph, and client surfaces are untouched during reload
- The old Lua state is closed, so a module must release what it registered with GLib or GDBus when `"exit"` is emitted, or its callback outlives the state it points into
- A source sweep in C and an LD_PRELOAD closure guard (`lgi_closure_guard.so`) back that contract up and report when it is broken. The sweep destroys any GLib source the closed state still owned, so it never dispatches; the guard cannot do the same, since libffi reads a closure's `cif` (which lives in the closed state) before the guard is entered

**Window Visibility Timing**
- X11: `xcb_map_window()` maps immediately, content shows when ready
- Wayland: Scene node not enabled until content is ready
- `drawin_refresh_drawable()` in `objects/drawin.c` enables the scene node once content exists
- Prevents visual smearing during initial render

**GTK Theme Detection**
- AwesomeWM's `beautiful/gtk.lua` creates actual GTK+ 3 widgets via LGI and queries `GtkStyleContext` for live theme colors
- SomeWM parses `~/.config/gtk-3.0/settings.ini` and `~/.config/gtk-4.0/settings.ini` directly, with Adwaita Dark as the fallback
- Theme detection is less accurate — complex GTK CSS that the file parser cannot read will be missed

**Xresources**
- AwesomeWM's `beautiful/xresources.lua` queries the X server's resource database via `xrdb`
- SomeWM's `gears/xresources.lua` parses `~/.Xresources` directly, falling back to Catppuccin Mocha defaults
- This means `Xft.dpi` and other resources work, but dynamically loaded resources (via `xrdb -merge`) won't be picked up

**Wibox Shape Surfaces**
- AwesomeWM uses 1-bit alpha masks for shape bounding/clip/input surfaces
- SomeWM uses full ARGB32 surfaces with `cairo.Antialias.BEST`, producing anti-aliased rounded corners
- Shape surfaces are scaled by `screen.scale` for HiDPI
- Surface references are retained (not finished) because the C side reads them asynchronously on Wayland, unlike X11 which copies immediately
- SomeWM adds a `shape_border` property on wibox for colored anti-aliased shape borders

**Window Type Handling**
- Native Wayland clients may not set a window type, resulting in `c.type == nil`
- SomeWM treats `nil` type as `"normal"` in `awful/client.lua` so focus rules and placement work correctly

---

## No-Op APIs

These APIs exist and can be called without error, but have no effect on Wayland.

| API | Status | Reason |
|-----|--------|--------|
| `awful.client.shape.update.all` | No-op | X11 Shape Extension unavailable on Wayland |
| `awful.client.shape.update.bounding` | No-op | X11 Shape Extension unavailable on Wayland |
| `awful.client.shape.update.clip` | No-op | X11 Shape Extension unavailable on Wayland |
| `screen._scan_quiet` | No-op | SomeWM derives `screen._viewports()` live from the wlroots monitor list; there is no separate viewport cache to re-scan |

### Client Shape (Rounded Corners)

Location: `luaa.c` require() hook, patched at load time

AwesomeWM uses the X11 Shape Extension (`xcb_shape_mask()`) to apply non-rectangular window shapes (e.g. rounded corners via `gears.shape.rounded_rect`). Wayland has no equivalent protocol-level feature. The `awful.client.shape.update.*` functions are replaced with no-ops via a require() hook so that user configs referencing `client.shape_bounding` or `client.shape_clip` load without error.

See `ideas/Shapes.md` for technical rationale and potential future approaches (shader-based clipping, custom render pass).

### Screen Scan (`screen._scan_quiet`)

Location: `objects/screen.c`

Upstream AwesomeWM calls `screen._scan_quiet()` from `awful.screen.dpi`'s `scanned` handler when that signal fires with no screens and no viewports; it re-runs the RandR/Xinerama scan to repopulate the viewport cache without re-emitting `scanning`/`scanned`, which would recurse into the caller.

SomeWM keeps no separate viewport cache: `screen._viewports()` derives its list directly from the compositor's live monitor list (`mons`), which the wlroots backend already keeps current. `screen._scan_quiet()` is therefore a documented no-op. Emitting `property::_viewports` from it would be wrong as well, since its Lua handler asserts a non-empty list and this path is only ever reached when the list is empty. The handler falls through to its own `create_screen_handler` / `fake_add` fallback, so a `scanned`-with-no-screens race no longer leaves the desktop with 0 screens and 0 tags. It exists only so configs and `dpi.lua` that call it load and run unchanged.

---

## Not Implemented (Stubs Only)

These APIs exist as stubs for compatibility but don't function:

| API | Status | Reason |
|-----|--------|--------|
| `awesome.register_xproperty()` | Stub | X11 property persistence doesn't exist on Wayland |
| `awesome.get_xproperty()` | Stub | X11 property persistence doesn't exist on Wayland |
| `awesome.set_xproperty()` | Stub | X11 property persistence doesn't exist on Wayland |
| `awesome.xkb_set_layout_group()` | No-op | Not yet wired to wlroots XKB state |
| `awesome.xkb_get_layout_group()` | Returns `0` | Not yet wired to wlroots XKB state |
| `awesome.xkb_get_group_names()` | Returns `""` | Not yet wired to wlroots XKB state |
| `root._string_to_key_code()` | Returns `0` | X11 keycode conversion; somewm uses xkbcommon keysyms directly |

### X Property APIs

The global stubs (`luaA_register_xproperty()`, `luaA_set_xproperty()`, `luaA_get_xproperty()` in `property.c`) and per-client stubs (`luaA_client_get_xproperty()`, `luaA_client_set_xproperty()` in `objects/client.c`) return "not yet implemented" warnings.

X11 properties were used for:
- Storing persistent per-window state
- Inter-client communication
- Session management

Wayland alternatives (not yet implemented):
- D-Bus for IPC
- Compositor-side storage for persistent state

### XKB Layout Functions

All three XKB Lua-facing functions in `xkb.c` are stubs. `xkb::map_changed` and `xkb::group_changed` signals do fire correctly, but the query/set APIs are not yet connected.

Multi-layout keyboard users should use `awful.input` to configure layouts at startup:
```lua
awful.input.xkb_layout = "us,ru"
awful.input.xkb_options = "grp:alt_shift_toggle"
```

Programmatic layout switching from Lua is not yet supported.

---

## Partially Implemented

| Feature | Status | Notes |
|---------|--------|-------|
| XKB toggle options | Layout set at startup only | `grp:alt_shift_toggle` etc. work at the XKB level but don't emit signals to Lua |
| Button press/release signals | Partial | `client::button_press` not fully emitted |
| Client `instance` property | Empty for Wayland | Wayland has no equivalent of `WM_CLASS` instance field |
| Client `machine` property | Empty for Wayland | Wayland has no `WM_CLIENT_MACHINE` equivalent |
| Client `icon_name` property | Empty for Wayland | No Wayland protocol provides this |
| `spawn::change` signal | Never emitted | Startup-notification progress not tracked on Wayland |
| `spawn::canceled` signal | Never emitted | Startup-notification cancellation not tracked |

### Client Properties for Native Wayland

The `instance`, `machine`, and `icon_name` properties are populated for XWayland clients (from X11 properties) but empty for native Wayland clients. The Wayland protocol does not provide direct equivalents.

For rule matching, use `class` (populated from the Wayland `app_id`) instead of `instance`:
```lua
-- AwesomeWM (X11): rule = { instance = "Navigator" }
-- SomeWM: use class instead
ruled.client.append_rule {
    rule = { class = "firefox" },
    properties = { tag = "web" },
}
```

---

## XWayland EWMH Gaps

These affect XWayland (X11) clients only. Native Wayland clients are not affected.

| Feature | Status | Impact |
|---------|--------|--------|
| `_NET_FRAME_EXTENTS` | Not sent | CSD-aware XWayland apps may misposition windows |
| `_NET_DESKTOP_GEOMETRY` | Hardcoded 1920x1080 | XWayland pagers/tools see wrong geometry on non-1080p monitors |
| `_NET_WM_DESKTOP` | Read but not applied | XWayland apps setting desktop before mapping land on wrong tag |
| Maximized combo | No h-max + v-max merging | XWayland apps requesting both get two state changes instead of one clean maximize |

These are tracked for future improvement. Most native Wayland apps are unaffected.

---

## Lua Layer Changes

These modifications to AwesomeWM's Lua libraries were necessary for Wayland compatibility:

| File | Change | Reason |
|------|--------|--------|
| `wibox/widget/systray.lua` | Complete rewrite | SNI D-Bus protocol replaces X11 XEmbed |
| `beautiful/gtk.lua` | Complete rewrite | File parsing replaces live GTK widget queries |
| `beautiful/init.lua` | `get_font_height()` returns 0 for unloadable fonts | Avoids hard errors from themes naming missing fonts |
| `wibox/init.lua` | ARGB32 shapes, HiDPI scaling, surface lifetime, `shape_border`; `border_width` reads 0 (not nil) when unset and border/opacity setters mirror into the drawin | Wayland scene graph and compositing model; C-side scene math needs concrete values |
| `wibox/drawable.lua` | HiDPI scale-change handler | Recreates surfaces when `screen.scale` changes |
| `wibox/hierarchy.lua` | `clip_child_extends` widget flag skips child-extent union | Needed by `wibox.layout.overflow` clipping |
| `awful/client.lua` | `c.type or "normal"` fallback; `awful.client.get_icon_path()` and desktop-entry icon lookup that sets `c.icon` at manage time | Native Wayland clients may not set window type and have no icon protocol (no `_NET_WM_ICON` equivalent) |
| `awful/widget/clienticon.lua` | Falls back to the desktop-entry icon path when the client provides no icon | Same missing Wayland icon protocol |
| `awful/widget/tasklist.lua` | Icon slot falls back to `awful.client.get_icon_path(c)`; templates can receive a file path string where upstream always passes a surface | Same missing Wayland icon protocol |
| `awful/widget/keyboardlayout.lua` | `next_layout`/`set_layout` wrap-around off-by-one fixed | Upstream cycles to an out-of-range group index |
| `awful/widget/layoutlist.lua` | `source.for_screen` returns `{}` for a nil screen instead of asserting | Screens can be absent mid hot-reload |
| `awful/permissions/init.lua` | Layer surface keyboard focus handlers; `request::tag` guards a nil `transient_for.screen` and applies `hints.tags` when the signal carries a list; hosts the focus restore and tag persistence handlers (see SomeWM-Only Features) | Wayland layer-shell, output hotplug and hot-reload have no X11 equivalent |
| `awful/mouse/snap.lua` | ARGB32 shapes, surface lifetime; edge snap dwell gating (see SomeWM-Only Features) | Same Wayland surface patterns as `wibox/init.lua` |
| `awful/root.lua` | `_remove_*` calls the C-side removal hook immediately when present | somewm's C layer exposes `_remove_` hooks; also flushes removals upstream leaves queued |
| `awful/screenshot.lua` | Snipping overlay wibox sets `surface_scale = 1.0` | HiDPI overlay repaint was ~1 FPS at physical resolution |
| `gears/filesystem.lua` | `somewm/` paths | Rebranded config/cache directories |
| `gears/timer.lua` | Records started timers in a weak-keyed table and removes their GLib sources when `awesome` emits `"exit"`; nothing new can be armed after that | A restart rebuilds the Lua state inside the running process, so a timer still armed would fire into a state that no longer exists. AwesomeWM re-execs instead and has nothing to release |
| `naughty/core.lua` | Default `request::icon` handler `naughty.app_icon_handler` resolves `app_icon` names via `menubar.utils.lookup_icon` | Upstream ships no default handler; naughty now requires menubar at load |
| `naughty/dbus.lua` | `awesome.version or "somewm-dev"` fallback; no `ActionInvoked("default")` on user dismiss; keeps the bus-name and object-registration ids so `"exit"` can hand them back | Version string safety; dismissing a notification should not open the app (reverts upstream 5daae2bb); without the release, `org.freedesktop.Notifications` stays owned by the state a restart replaced and notifications stop arriving |
| `naughty/notification.lua` | Property getter falls back to `beautiful.notification_*` between preset and defaults; presets no longer merge in `config.defaults`, so `n.preset.<field>` can be nil | Upstream ignores `beautiful.notification_*`; merged defaults shadowed the beautiful lookup |
| `naughty/widget/icon.lua` | Clears the image when a notification's icon is unset; disconnects the signal it actually connects | Upstream leaves a stale image and leaks the signal connection |
| `naughty/layout/box.lua` | Caches the notification position at attach time for use after the weak ref is GC'd | Upstream falls back to a hardcoded `"top_right"` |
| `gears/color.lua` | `parse_color` and `ensure_pango_color` accept the CSS functional forms `rgb(r,g,b)` and `rgba(r,g,b[,a])` | Matches the compositor's `color.c` parser (integer 0-255 or 0.0-1.0 channels, fraction alpha), so one color syntax works across `gears.color`, Pango markup and compositor properties |
| `wibox/widget/textbox.lua` | `set_markup_silently`/`get_markup_geometry` rewrite `rgb()`/`rgba()` color attributes to `#rrggbbaa` before `Pango.parse_markup` | Pango only understands hex/named colors; without this, markup embedding an `rgba()` color (tasklist, menu, prompt, naughty, lockscreen) fails to parse and renders invisible |

### Removed globals

| Global | Replacement | Reason |
|--------|-------------|--------|
| `_timer` | `gears.timer` | An undocumented C wrapper around `wl_event_loop_add_timer` with no callers in the tree. Its timers were never removed on a hot-reload, so each one outlived the Lua state that owned it. `gears.timer` is GLib-based and unaffected. |

### New Lua Modules (no AwesomeWM equivalent)

| Module | Purpose |
|--------|---------|
| `awful.input` | Libinput pointer/keyboard configuration |
| `awful.ipc` | Unix socket IPC for `somewm-client` |
| `awful.systray` | D-Bus StatusNotifierHost |
| `awful.statusnotifierwatcher` | D-Bus `org.kde.StatusNotifierWatcher` |
| `wibox.widget.systray_icon` | Individual SNI icon widget |
| `ruled.layer_surface` | Rules for layer-shell surfaces (panels, launchers) |
| `gears.xresources` | File-based Xresources parser |
| `gears.bitwise` | Pure-Lua bitwise operations |
| `awful.layout.suit.carousel` | Scrollable tiling layout (horizontal and vertical) |
| `awful.gesture` | Touchpad gesture bindings (swipe/pinch/hold) |
| `awful.test_marker` | Test-only keybind remapping, active only under `SOMEWM_TEST_NAME` |
| `lockscreen` | Built-in lock screen (used by `somewmrc.lua`) |
| `debug.focus_tracker` | Debug overlay showing focus/tag/cursor state |
| `wibox.layout.overflow` | Scrollable widget layout, port of AwesomeWM PR #3309 (uses the `clip_child_extends` hook in `wibox/hierarchy.lua`) |
| `somewm` | Lazy-loaded namespace for somewm-only Lua modules |
| `somewm.layout_animation` | Animated tiling transitions (mwfact, layout switch, spawn/kill) |
| `fx.autocolor` | Dominant-color titlebar UI: samples `client.content` (a compositor-only scene readback), derives a readable foreground via WCAG luminance, and commits the color instantly. Reads the pixel strip back through a temp PNG (`write_to_png` + `new_from_file`); see the module header for the full readback rationale |

---

## SomeWM-Only Features

These features are unique to somewm and don't exist in AwesomeWM:

### `awful.input` - Input Device Configuration

18 properties for pointer and keyboard settings:

```lua
local awful = require("awful")

-- Pointer settings
awful.input.tap_to_click = 1
awful.input.natural_scrolling = 1
awful.input.accel_speed = 0.5
awful.input.scroll_button = 274  -- Middle mouse
awful.input.left_handed = 0

-- Keyboard settings
awful.input.xkb_layout = "us"
awful.input.xkb_variant = ""
awful.input.xkb_options = "ctrl:nocaps"
awful.input.xkb_model = "pc105"
awful.input.xkb_rules = "evdev"
awful.input.repeat_rate = 25
awful.input.repeat_delay = 600
```

### NumLock on Startup

Wayland compositors start with NumLock off by default. AwesomeWM has no equivalent API because X11 inherits NumLock state from the display server.

Enable NumLock at startup from `rc.lua`:

```lua
awesome._set_keyboard_setting("numlock", true)
```

`some_set_numlock()` in `somewm_api.c` toggles the Mod2 locked modifier mask via `wlr_keyboard_notify_modifiers()` on all member keyboards (same pattern as Sway's `input * xkb_numlock enabled`).

NumLock (Mod2) is automatically stripped from `CLEANMASK` so keybindings and wibar scroll bindings work correctly whether NumLock is on or off.

---

### `somewm-client` - IPC CLI Tool

~45 commands for external control:

```bash
somewm-client ping                    # Health check
somewm-client client list             # List windows
somewm-client client focus <id>       # Focus window
somewm-client input tap_to_click 1    # Set input property
somewm-client eval "return 1+1"       # Eval Lua
somewm-client screenshot              # Take screenshot
```

### `output` - Physical Monitor Object

The `output` object represents a physical monitor connector (HDMI-A-1, DP-2, eDP-1). Unlike `screen` objects (which are destroyed on disable and recreated on enable), output objects persist from plug to unplug.

```lua
-- Iterate outputs
for o in output do
    print(o.name, o.make, o.enabled)
end

-- Configure by hardware
output.connect_signal("added", function(o)
    if o.name:match("^eDP") then
        o.scale = 1.5
    end
end)

-- Access from a screen
local o = screen.primary.output
```

AwesomeWM has no equivalent because X11 delegates monitor management to `xrandr`. See `objects/output.c`.

### `screen.scale` - Fractional Output Scaling

Set output scale dynamically from Lua or CLI. `screen.scale` delegates to `output.scale` as a single source of truth.

```lua
-- Lua API (both are equivalent)
screen.primary.scale = 1.5
screen.primary.output.scale = 1.5
```

```bash
# CLI
somewm-client screen scale           # Get focused screen scale
somewm-client screen scale 1.5       # Set focused screen to 1.5
somewm-client screen scale 1 1.5     # Set screen 1 to 1.5
```

Apps supporting `wp_fractional_scale_v1` render at native resolution. Struts/workarea are automatically recalculated after scale changes.

### `screen.content` - Screenshots

Capture screen contents from Lua:

```lua
local surface = screen.primary.content
```

### `client:dominant_color()` - Compositor-Side Dominant Color

The compositor computes a client's dominant color directly from its scene
tree, without a Lua readback. The method composites the content into a
`content_width x rows` cairo strip and runs a dependency-free C histogram
(`dominant_color.h`) on the strip's pixels:

```lua
local hex, share = c:dominant_color { rows = 12, step_x = 2, step_y = 1 }
```

- `rows` is in logical pixels from the top of the content; nil or 0 means the
  whole content. The ROI support in the shared screenshot composite skips
  buffers outside the strip and, for NORMAL DMA-BUF transforms, reads back
  only the buffer rows that can land in it.
- `step_x`/`step_y` (default 2/1) control the sample grid; for a whole-content
  sample a `thumb` option (approximate sample-grid size) derives the steps so
  roughly `thumb x thumb` pixels vote.
- Returns `"#rrggbb"` and the winning-bin share, or nothing when nothing
  painted or nothing voted.

This is what `fx.autocolor` uses; see its module header for the sampling cost.

### Additional Client Properties

| Property | Description |
|----------|-------------|
| `client.id` | Unique compositor-assigned client ID |
| `client.aspect_ratio` | Client aspect ratio hint |
| `client.shadow` | Per-client shadow toggle |
| `client.backdrop_blur` | Client backdrop blur (see Backdrop Blur below) |

Wiboxes and drawins have a matching `shadow` property; wibars also read the `beautiful.wibar_shadow` theme variable.
Layer surfaces have `layer_surface.backdrop_blur`. `client.corner_radius`
and `layer_surface.corner_radius` are already listed elsewhere.

### Shadows

Compositor-level drop shadows for clients and wiboxes (no AwesomeWM
equivalent; under X11 this is picom's job). Configured through
`beautiful.shadow_*`, per-object via `client.shadow` / wibox `shadow`.

The shadow is the object's frame grown by `spread`, moved by
`offset_x`/`offset_y`, rounded by `corner_radius`, fading out over
`radius` pixels. `color` accepts `#RRGGBBAA`; its alpha multiplies
`opacity`.

By default the shadow follows the window's rounded-corner shape: it adopts
the object's effective per-corner radii (`corner_radius` on the client/drawin,
`beautiful.corner_*`), so a window with, say, only its top corners rounded
casts a shadow with exactly that contour. `corner_radius` on the shadow
config itself accepts a per-corner table (`{ top_left = N, ... }`,
`{ corner_radii = {tl, tr, bl, br} }`, or a plain number for all four).
On the per-object `shadow` table, naming explicit radii switches
`follow_corners` off for that object; on the theme side, set
`beautiful.shadow_follow_corners = false` (or `shadow_drawin_follow_corners =
false`) so every shadow uses its own `shadow_corner_radius` instead of
following the windows. Per-corner `shadow_corner_radius` values are honoured.

Changed in 1.4.3: `clip_directional` no longer has an effect. The shadow
is drawn at its offset and fades out on every side; sides fully covered
by the window are simply not visible. Configs that set it still parse.

In a SceneFX build the shadow is one `wlr_scene_shadow` node instead of the
CPU nine-patch; `beautiful.shadow_*` and the per-object `shadow` tables drive
it unchanged. SceneFX takes a single `corner_radius` for all corners, so the
widest configured corner wins there; the per-corner radii from
`follow_corners` / per-corner `shadow_corner_radius` are ignored in a
SceneFX build (still honoured without SceneFX).

### Backdrop Blur

Opt-in compositor backdrop blur (no AwesomeWM equivalent; think the blurred
wallpaper behind a translucent terminal). Configurable per object:

| Property | Description |
|----------|-------------|
| `client.backdrop_blur` | Client backdrop blur toggle/config |
| `layer_surface.backdrop_blur` | Layer-shell panel blur toggle/config |

Accepted values:

- `true` / `false` — blur on/off. On, the blur adopts the object's corner
  radii (the same effective radii the corners use).
- a table `{ corner_radius = N, alpha = A, strength = S }` — a uniform
  radius override (default: the object's radii), plus the blur's alpha and
  strength (both default 1.0).

The blur is a `wlr_scene_blur` node under the object's content, same box and
radii, with the content buffer as its transparency-mask source, so fully
transparent pixels stay sharp. It is off while a client is fullscreen and is
re-applied on every surface commit (wlroots detaches the mask link when a
new buffer is attached). Global blur device parameters default to
`num_passes = 2, radius = 5`, overridable once per session from Lua with
`awesome.set_blur_data(num_passes, radius, noise, brightness, contrast,
saturation)`.

SceneFX-specific notes: only the plain blur node is used; the optimized
blur path (`wlr_scene_optimized_blur`, `should_only_blur_bottom_layer`) is
not enabled because it has no cache and would re-blur every frame. Layer
surfaces only get blur via the property above — there is no automatic blur
behind every panel.

### Inner Hairline (macOS-style inner border)

A thin RGBA line drawn by the compositor just inside the window edge, on top
of the content and inside the (optional) outer border. No AwesomeWM
equivalent. For clients it traces the whole decorated frame (surfaces plus
attached titlebars) as one contour; for drawins/wiboxes it hugs the content
edge. It is pure decoration: input-transparent, and it changes no geometry.

Theme variables (shared width/color, separate toggles):

| Variable | Meaning |
|----------|---------|
| `beautiful.border_inner_enabled` | Enable for clients |
| `beautiful.border_inner_drawin_enabled` | Global fallback for every drawin/wibox at creation |
| `beautiful.border_inner_width` | Line width; `0` disables the hairline |
| `beautiful.border_inner_color` | `#RRGGBBAA` (alpha used as-is, no focus dimming) |

Per-surface overrides, mirroring the outer border's `wibar_border_*` keys:

| Variable | Surface |
|----------|---------|
| `beautiful.wibar_border_inner_{enabled,width,color}` | `awful.wibar` only |
| `beautiful.wibox_border_inner_{enabled,width,color}` | any plain `wibox` |

A wibox resolves the hairline as: explicit `wibox{border_inner_*}` args →
`wibar_border_inner_*` (wibars) → `wibox_border_inner_*` → the global
`border_inner_drawin_enabled` fallback. Per-object `set_border_inner_*` and
client rules still work; the Lua side keeps a `_user_*` flag so a default
keeps tracking the theme until something overrides it. In `somewm.c` the
compiled defaults are width `1`, `rgba(255,255,255,0.10)`, both toggles off.

### Edge Snap Dwell

`awful.mouse.snap.snap_dwell_ms` (default 150) delays the edge-snap placeholder until the cursor has dwelled in the same edge zone for that many milliseconds, preventing accidental snaps during fast drags. Set to 0 for AwesomeWM's immediate behavior.

### Focus Restore and Tag Persistence

When a lock screen, layer surface, or output change releases focus, the compositor emits the somewm-only `request::focus_restore` screen signal. `awful.permissions.focus_restore` handles it by activating the best client from focus history.

When a screen is removed, `awful.permissions.tag_screen` saves its tag state (name, selection, layout, master settings, clients) into `awful.permissions.saved_tags`, keyed by output name. `somewmrc.lua` restores the saved state when the output reconnects.

A hot-reload (`awesome.restart()`) re-runs `rc.lua` in the same process, so every tag is built from scratch. Before the old Lua state closes, the compositor records each tag's screen and name, whether it was selected, which clients were on it, and the state `awful` keeps for it in Lua: the layout name, `master_width_factor`, `master_count`, `column_count` and `gap`. After `rc.lua` has run, each recorded tag is matched to the first new tag on the same screen with the same name, or to nothing. There is no fallback by position: a tag renamed in `rc.lua` loses its clients to the rules, and a tag created at runtime (a `new_tag` rule, a volatile tag) never lands on a config tag.

For each matched tag, the compositor selects the first same-named layout from the rebuilt tag's allowed `layouts` list, and puts the four numbers back, before it restores clients or selection. Layout objects belong to their Lua state and cannot be reused directly. If the old layout has no named match in the new list, the tag keeps the layout selected by `rc.lua`. Only what the old session actually set is recorded, so a property it left alone comes back from the new config and theme instead of from the old session.

For each client with at least one match, the compositor emits `request::tag` on the client before the rules run, with the first matched tag as the tag argument and hints of:

```lua
{ reason = "restart", tags = { <every matched tag> } }
```

`awful.permissions.tag` applies `hints.tags` when present, so a client on several tags comes back on all of them. AwesomeWM does the same thing on its own restart through `_NET_WM_DESKTOP`, but restores only the first tag and does not restore the selection. The rules then run as on AwesomeWM's restart: a rule with no `tag` property emits `request::tag` with reason `"rules"` and the handler returns early because the client already has tags, a `tag =` rule replaces the restored tags, a `tags =` rule merges only when given tag objects on the client's screen and replaces when given names, and a `screen =` rule pointing at another screen discards the restore. A client whose tags all fail to match gets no signal and is placed by the rules as if it were new.

The selection is restored next, still before the rules run, so a client the rules place on the selected tags lands on the restored selection. Per screen with at least one recorded selected tag that matched, every tag on the screen is set selected or not to match the record, then `tag::history::update` is emitted once, so `awful.tag`'s history holds the restored set and nothing in between. No `request::select` is emitted. A screen whose selected tags matched nothing keeps the selection `rc.lua` made.

A client's floating state is restored before the rules run, so a rule with a `floating` property still wins. Only a floating state something set explicitly is restored: a client that floats because of its type floats again for the same reason.

Titlebars are rebuilt by the rules; their old extents are removed before their Lua drawables are discarded so recreating the same titlebars does not grow client geometry across reloads.

### Cursor Theming

```lua
root.cursor_theme("Adwaita", 24)   -- Set cursor theme and size
root.cursor_size()                  -- Get current cursor size
```

### SNI Systray

Modern D-Bus tray protocol instead of X11 embed. Implementation:
- `objects/systray.c` - C object and D-Bus watcher
- `lua/awful/statusnotifierwatcher.lua` - Lua bindings
- `wibox.widget.systray` - Widget (rewritten from AwesomeWM's X11 version)

### Carousel Layout

A niri-inspired scrollable tiling layout with no AwesomeWM equivalent. Clients are arranged in columns on an infinite horizontal (or vertical) strip, with the viewport auto-scrolling to keep the focused column visible.

**Layout registration:** `lua/awful/layout/suit/init.lua` is modified to include `carousel = require("awful.layout.suit.carousel")`. This is the only change to a Sacred Lua file in this feature.

**New Lua APIs (underscore-prefixed, internal use):**

| API | Object | Purpose |
|-----|--------|---------|
| `client:_set_geometry_silent(geo)` | client | Set geometry without emitting signals or reassigning screens. Used by layouts that position clients offscreen (e.g. scrolling). |
| `awesome.start_animation(duration, easing, tick_fn, done_fn)` | awesome | Frame-synced animation with easing. Returns a handle with `:cancel()` and `:is_active()`. |

**C-side changes:**
- `client_resize()` gains a `silent` parameter to skip signal emission and screen reassignment
- `commitnotify` in `somewm.c` skips `resize()` for tiled clients so offscreen positioning is not clamped
- `animation.c` provides the C-side animation tick loop, integrated into `some_refresh()`

### `somewm.*` - SomeWM-Only Lua Namespace

Lazy-loaded namespace for somewm-specific Lua modules that have no AwesomeWM equivalent. Submodules live under `lua/somewm/` and are loaded on first access via `require("somewm")`.

### `somewm.layout_animation` - Animated Layout Transitions

Hooks into `screen::arrange` and smoothly animates tiled clients from their previous geometry to the new one. Covers all arrange triggers: mwfact changes, client spawn/kill, layout switches, column count changes.

```lua
local layout_anim = require("somewm.layout_animation")
layout_anim.duration = 0.15          -- seconds
layout_anim.easing   = "ease-out-cubic"
layout_anim.enabled  = true           -- default
```

Animation is skipped when disabled, during mousegrabber (direct manipulation), when the geometry delta is negligible (< 2px), or on a client's first arrange.

### Layer Surface Rules

Wayland layer-shell surfaces (panels, launchers, overlays) can be matched with rules:

```lua
ruled.layer_surface.append_rule {
    rule = { namespace = "launcher" },
    properties = { keyboard_interactivity = "exclusive" },
}
```

---

## Testing Implications

Some AwesomeWM tests won't work due to these deviations:

| Test Pattern | Issue | Workaround |
|--------------|-------|------------|
| X property tests | APIs are stubs | Skip or use D-Bus alternatives |
| Keygrabber release tests | Only press events sent | Skip release-dependent tests |
| XKB layout switching tests | Layout query/set APIs are stubs | Test via `awful.input` instead |
| `instance`-based rule tests | Empty for Wayland clients | Use `class` matching instead |

---

## Future Work

Potential future compatibility improvements:

1. **XKB layout functions** - Wire `xkb_set_layout_group()` / `xkb_get_layout_group()` / `xkb_get_group_names()` to wlroots XKB state
2. **Property storage** - Compositor-side persistent state for clients
3. **Session management** - Wayland-native session protocol support
4. **EWMH frame extents** - Send `_NET_FRAME_EXTENTS` to XWayland clients
5. **EWMH desktop geometry** - Report actual output geometry instead of hardcoded 1920x1080
