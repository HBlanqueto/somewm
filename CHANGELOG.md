# Changelog

All notable changes to somewm will be documented in this file.

## [Unreleased]

### Added

- Opt-in compositor backdrop blur behind `client.backdrop_blur` and
  `layer_surface.backdrop_blur` (a SceneFX build; no-op without it). The
  `wlr_scene_blur` node follows the object's box and corner radii, uses the
  content buffer as its transparency mask and switches off while a client is
  fullscreen. Global device parameters are set with
  `awesome.set_blur_data(num_passes, radius, noise, brightness, contrast,
  saturation)`.

- Shadows follow the window's rounded corners: they adopt the object's
  effective per-corner radii (`follow_corners`, on by default), and
  `shadow.corner_radius` accepts a per-corner table
  (`{ corner_radii = {tl, tr, bl, br} }`, named keys, or a plain number)
  with theme support via `beautiful.shadow_follow_corners` and per-corner
  `shadow_corner_radius`

- The aerosnap placeholder is now themeable end to end: it can drop a shadow
  (`beautiful.snap_shadow`, on by default), draw the macOS-style inner hairline
  (`beautiful.snap_border_inner`, on by default, using the same
  `wibox_border_inner_*` width/color keys), and round its corners to match the
  dragged window (`beautiful.snap_follow_corners`, on by default, reusing the
  same per-corner radii the window's own shadow follows via
  `shadow_corner_radius`).  With follow-corners off, the preview falls back to
  the themed `shadow_corner_radius`, mirroring the shadow's own behaviour

- The `roundy` theme now ships a flat solid wallpaper instead of
  `background.png`: `theme.wallpaper_colors` renders as a solid fill (two
  identical stops) using the wibar's `bg_normal`, and the SomeWM "S" logo is
  drawn on top in the theme's blue accent via `theme.wallpaper_logo_color`.
  The stock `request::wallpaper` handler already draws the logo (as the
  Catppuccin theme does); the logo asset is `icons/somewm-logo.svg`

- Optional geometry tracer for diagnosing windows that move unexpectedly:
  setting `SOMEWM_TRACE=1` makes the compositor log every client geometry
  change to stderr as `[TRACE-GEO]` — the app, the old and new rectangle,
  the delta, and the C backtrace plus Lua call stack that produced it — and
  mark every pointer press as `[TRACE-BTN]` (which widget it hit, cursor,
  focused surface, mousegrabber state). Off by default and zero-cost when
  unset; see `docs/debugging.md`

- `client:dominant_color()`: the compositor computes a client's dominant
  color in C (`dominant_color.h`, a dependency-free ARGB32 histogram) from a
  `content_width x rows` strip. The shared screenshot composite gained an
  optional region of interest that skips buffers outside the strip and, for
  NORMAL DMA-BUF transforms, reads back only the buffer rows that can land in
  it. `rows` = 0 samples the whole content, optionally stepped so roughly
  `thumb x thumb` pixels vote

### Changed

- Autocolor live mode no longer re-samples idle clients: once the compositor
  has delivered a `surface::commit`, the interval pump only runs for a client
  that is dirty or still within a small post-commit trailing budget (which
  covers stale XWayland snapshots). Builds without the commit hook keep
  interval polling. Idle clients do no readback.

- Autocolor samples through `client:dominant_color()` instead of reading
  `c.content` back through a temp PNG: the compositor composites only the
  sampled strip and runs the C histogram on it, so a sample no longer
  allocates and composites the whole client or walks a Lua per-pixel loop.
  Compositor builds without the method keep the bound fallback color (one
  warning per session)

### Fixed

- A client with per-corner rounded corners no longer keeps its bottom-left
  corner square: the crop's bottom-left arc centre used the top-left radius
  (`rounded_crop_pixels`)

- Tasklists using `awful.widget.clienticon` as their `icon_role` no longer
  crash with `attempt to call method 'set_image' (a nil value)` once a client
  has an icon: `awful.widget.common` only calls `set_image` when the icon_role
  widget actually provides it (clienticon draws the icon itself from the
  client)

- Autocolor's old cairo crop is gone with the temp-PNG readback. It was
  broken by construction: `mode_from_surface` called `cr:scale` after
  `set_source_surface`, but cairo locks the source matrix at set-source time,
  so nothing scaled and `top` read only the left half of the first 12 rows
  while `full` read only the top-left corner. The compositor-side sampler
  composites the strip directly, so this cannot recur
- The titlebar foreground now uses WCAG relative-luminance contrast instead of
  a flat luminance cutoff, and flips immediately with the committed colour

- Fresh Wayland clients no longer map at `(0,0)`, tucked under a top wibar:
  `screen_update_workarea()` now syncs `Monitor.w` (the workarea the C manage
  path reads) when a drawin strut changes it, and a fresh XDG toplevel is
  seeded at its target monitor's workarea origin. Configs without a placement
  rule (e.g. a minimal `rc.lua`) are affected; the stock `somewmrc.lua` global
  `no_overlap+no_offscreen` rule already masked it

- Drop shadows are input-transparent: their slices and interior fills set
  `point_accepts_input = false` (the interior fills are now scene buffers
  sharing one 1×1 solid texture, since `wlr_scene_rect` cannot reject input).
  Clicking near a window edge — under its own or a neighbour's shadow — no
  longer focuses the wrong window or falls through the layer

- A hot reload no longer double-drops the shadow's shared fill buffer:
  `clients_detach()` (luaa.c) now NULLs `c->shadow.fill_buf` alongside the
  nine-patch textures and the crop buffers, so the dying Lua state's GC can
  never free a `wlr_buffer` the snapshot still owns (the failure mode was
  `wlr_buffer_drop: Assertion '!buffer->dropped' failed.`)

- A `scanned`-with-no-screens race after a hot reload no longer leaves the
  desktop with 0 screens and 0 tags. `screen._scan_quiet()` existed upstream
  but was missing here, so `awful.screen.dpi`'s `scanned` handler aborted
  mid-scan; it is now a documented no-op (SomeWM derives
  `screen._viewports()` live from the wlroots monitor list, so there is no
  separate cache to refresh) and the handler falls through to its own
  `fake_add` recovery instead

### Removed

- The autocolor fade animation: colors are committed instantly
  (`beautiful.autocolor_fade` / `custom_ac_fade` are no longer read)

- Autocolor's temp-PNG / GdkPixbuf readback (`os.tmpname`,
  `cairo.write_to_png`, `GdkPixbuf.Pixbuf.new_from_file`) and its Lua
  per-pixel tally, replaced by the compositor-side `client:dominant_color()`

## [1.4.5] - 2026-09-01

Patch release. 10 commits since 1.4.4: shadows, config checking, and two build
fixes. No API changes; existing rc.lua configs run unchanged.

### Added

- Shadows take `spread`, `corner_radius`, and an alpha channel on their colour
- `somewm --check` detects a GTK application built inside the compositor,
  `Gtk.IconTheme.get_default()`, bare `xrdb`, client icon use, and
  `xsettingsd`. Every finding it can explain now prints a link to the section
  of the migration guide that explains it

### Fixed

- `somewm --check` and the startup scan read the whole config. A `require()`
  whose name had no dot was treated as a standard library and skipped, so a
  config split across modules was never read past its `rc.lua`
- A pattern found during startup no longer refuses the config. It reports and
  loads; a config that genuinely hangs is still caught by the load alarm
- Only the first occurrence of each pattern in a file was examined, so a
  mention in a comment hid the real use below it
- A line matching several patterns for one tool was reported once per pattern
- `require("pkg.mod." .. name)` left a trailing dot, which resolved to a `//`
  path and scanned the same file twice
- The `xset` patterns matched `xsettingsd`, which is a different program
- A missing module is reported at the line of its `require`, not line 0
- `awesome.get_xproperty` and `awesome.set_xproperty` are not defined, so
  calling either one aborts the config. `--check` ranks them critical and points
  at `awesome.startup`
- A shadow with a diagonal offset no longer shows a darker square at the corner
  where both fill strips overlapped, and no longer cuts off hard at the corners
- Fractional placement margins no longer grow an attached drawable by a pixel
  per pass until the C stack overflows. Port of AwesomeWM PR #4122
- PAM is found on distributions that ship no `pam.pc`, by looking for the header
  and library directly
- Builds on 32-bit architectures, where comparing `lua_Integer` against
  `UINT32_MAX` raised `-Wsign-compare`
- `somewm-client test start` keeps its state directory when a start fails, so
  the log it names is still there to read

### Changed

- Shadows are drawn as the frame grown by `spread`, moved by its offset, and
  faded over the radius on every side with rounded corners. `clip_directional`
  no longer has an effect
- `SOMEWM_TEST_KEEP_FAILED` is removed. A failed start always keeps its state
  directory, and the next start under that name clears it

## [1.4.4] - 2026-08-20

Patch release. 10 commits since 1.4.3: client popups, input routing, and idle
inhibitors. No API changes; existing rc.lua configs run unchanged.

### Fixed

- Client popup menus open where the application asked, instead of offset up and
  left by the border width and titlebar height, and no longer get cropped at the
  client's content edge. Context menus routinely open above and left of the
  pointer, so this was visible in Firefox and anything else with a context menu
- Popups paint above client borders and shadows instead of behind them
- X11 clients keep the pointer grab when you click the window that already has
  focus, so games no longer lose mouse capture on every click
- Scroll ticks reach the mousegrabber as buttons 4/5, as they do in AwesomeWM.
  BTN_SIDE and BTN_EXTRA no longer land in those slots; they are buttons 8/9
- Clicking a wibar no longer fires the root button binding on top of whatever
  the wibar did with the click
- Pointer focus unpins on the seat's button count instead of a local latch, so a
  release swallowed by a mousegrabber no longer pins focus to a stale surface
- Destroying an idle inhibitor recomputes after wlroots unlinks it, so idle
  timers are no longer latched off for the rest of the session
- Systray icons size by the theme's `base_size` rather than the source pixmap,
  so an app publishing a 256x256 icon no longer takes a 256px wide slot with the
  icon letterboxed inside it
- Tag names that are empty or unset no longer read through a null pointer

## [1.4.3] - 2026-08-08

Patch release. 25 commits since 1.4.2: a hot-reload rework, wlroots 0.20
support, and border geometry fixes. One property removed to match upstream
AwesomeWM; existing rc.lua configs otherwise run unchanged.

### Added

- wlroots 0.20 support, alongside 0.19. The build picks whichever version the
  system provides; `-Dwlroots_version` forces the choice. Restores the build
  on Debian 13 and Ubuntu 24.04 (#579)
- Restart test suite: assertions on both sides of a real `awesome.restart()`,
  driven from outside through `somewm-client test`
- `make check-qa` runs luacheck, mirroring upstream AwesomeWM

### Fixed

- Hot-reload closes the old Lua state instead of leaking it, ending the
  ~90 MB RSS growth per `awesome.restart()` (#574)
- Hot-reload no longer crashes in lgi after reload: lgi's C libraries are
  pinned across the state close (#465)
- naughty and the status notifier watcher release their D-Bus names and
  registrations on exit, so notifications and the systray survive reloads
- Both rebuild paths reset C-held Lua state, including the config-timeout
  recovery path
- Border width no longer inflates client size: geometry is border-exclusive
  as in AwesomeWM, fixing titlebar positioning, shadow sizing, clipping, and
  `c.content` capture offsets
- Destroying the focused client emits `unfocus` and `property::active`,
  matching AwesomeWM
- Timers armed during the refresh cycle wake the poll, so an idle session no
  longer sleeps past a due `gears.timer`
- The seat advertises pointer and keyboard capabilities at startup, not only
  after the first input device appears
- `print()` from rc.lua reaches redirected logs (stdout is line-buffered)
- awful.ipc parses under Lua 5.5 (loop variables are read-only there)
- tasklist and drawable no longer swallow widget errors; undefined-variable
  bugs fixed in ipc, systray tooltips, and focus_tracker
- `clickfinger_button_map` initialized (touchpad clickfinger crash)

### Removed

- `client_shape_input` property and `awful.client.shape.update.input`,
  matching upstream AwesomeWM's revert of the feature (#4100). The drawin
  `shape_input` property is unaffected.

### Notes

- AwesomeWM baseline unchanged from 1.4.0, plus ported upstream fixes #4100
  and #3998.
- See [`DEVIATIONS.md`](DEVIATIONS.md) for Wayland vs X11 differences.

## [1.4.2] - 2026-06-22

Patch release. 43 commits since 1.4.1: mostly bug fixes, plus a few additive
features. No public API breaks; existing rc.lua configs run unchanged.

### Added

- `somewm::ready` and `xwayland::ready` signals, also exposed as `awesome.*`
  properties and re-emitted on hot-reload
- `-c NONE` to start without loading any user config
- XKB keyboard model and rules selection
- `screenshot` interactive (snipping) subcommand in the somewm-client CLI

### Fixed

- Compositor terminates cleanly on SIGTERM/SIGINT and Ctrl-Alt-Backspace
- Timer-driven widget redraws now present on an otherwise idle session (clocks update)
- Client buffer flush deferred out of the map signal emit (disconnect-mid-map crash)
- Titlebar hover no longer leaks pointer events to the client beneath it
- Aerosnap placeholder deferred by dwell, removing cross-monitor flicker
- `request::tag` nil-guard restored for `transient_for.screen`
- `createmon()` bails cleanly when an output commit fails (partial hotplug)
- Per-client geometry signals emitted on the xdg fullscreen path
- Hot-reload assigns all client screens before emitting restore signals
- Drawin struts aggregated per explicit screen pointer (multi-monitor)
- `c.content` captured via scene-tree walk and scaled to logical size on HiDPI
- Screenshot snipping overlay rendered at logical resolution

### Changed

- Default build is optimized release with no sanitizers (packaging-relevant)
- Build supports Lua 5.5; added a `lua_pkg` override
- Cleaned `-Werror` failures under GCC 15/16; stopped propagating `-Werror`
  into the wlroots, v4l-utils, and libdisplay-info subprojects

### Notes

- AwesomeWM baseline unchanged from 1.4.0.
- See [`DEVIATIONS.md`](DEVIATIONS.md) for Wayland vs X11 differences.

## [1.4.1] - 2026-04-24

Patch release. 19 commits since 1.4.0, all bug fixes and one additive
signal change. No public API breaks; existing rc.lua configs run unchanged.

### Fixed

- Use-after-free of `wlr_scene_tree` via `wlr_surface->data`
- SEGV on monitor unplug from missing surface cleanup (#442)
- `createmon()` hardening for partial monitor init failures (#477)
- Carousel: clients render across monitors when outside the carousel layout
- Borders stay visible when a client is partially offscreen
- Pointer enter delivered to newly mapped layer-shell surfaces
- Pointer focus re-evaluated after the banning refresh
- `LyrOverlay` placement preserved for override-redirect clients (XWayland stacking)
- wibox: opacity and border properties propagate to the underlying C drawin
- wibox: A1 surface format restored for shape masks
- Idle-inhibit exclude mechanism honored again (#446)
- Icon resolution falls back to `.desktop` entries when class isn't a theme icon
- Keygrabber stops key repeat when starting mid-press
- Keyboard layout `next_layout()` off-by-one in wrap-around
- README links point at the 1.4 docs site

### Changed

- The `exit` signal now carries a `restart` boolean argument: `true` on
  hot-reload, `false` on shutdown. Matches AwesomeWM behavior. Existing
  handlers that ignore arguments are unaffected.

### Notes

- AwesomeWM baseline unchanged from 1.4.0.
- See [`DEVIATIONS.md`](DEVIATIONS.md) for Wayland vs X11 differences.

## [1.4.0] - 2026-04-07

First stable release. SomeWM 1.4 = AwesomeWM 4.4 on Wayland.

### Added

- In-process Lua hot-reload for `awesome.restart()` - tears down and rebuilds the Lua VM while Wayland clients survive (#366)
- Carousel layout - niri-style scrollable tiling with per-column focus (#351)
- Animated tiling transitions with configurable easing and duration (#362)
- Lock screen with PAM authentication (#201)
- IPC client (`somewm-client`) with ~45 commands, event subscription, and shell completions (#338)
- First-class `output` object for Wayland monitor management (#290)
- Tag persistence across monitor hotplug (#312)
- Overflow layout with scrollbar support (#370)
- Per-device input rules via `awful.input.rules`
- Lua-settable idle inhibition via `awesome.idle_inhibit`
- Gesture module (`awful.gesture`) with wlr_pointer_gestures_v1 support
- Layer surface Lua API for layer-shell surfaces
- Screen fractional scaling via `screen.scale`
- Level-aware logging with `--verbose` flag (#191)
- Improved `--check` mode with suppression, severity filter, and GTK detection
- Systemd service units and session wrapper for distro packagers
- XDG desktop portal file for xdg-desktop-portal-wlr screen sharing
- Wallpaper caching for instant tag switching
- Libinput touchpad/trackpoint configuration
- Client aspect ratio constraints for resize
- `awesome.startup` property (#253)
- Runtime cursor theme and size configuration

### Fixed

- XWayland dialogs now float correctly via `_NET_WM_WINDOW_TYPE` (#337, #364)
- `screen:disconnect_signal()` implemented (#363)
- `root._remove_key()` implemented for dynamic keybinding removal (#405)
- EWMH `_NET_CLIENT_LIST_STACKING` updates enabled in stack operations (#406)
- Keygrabber release events now fire with `"release"` event type (#409)
- Crash when closing foot near XWayland clients (#386)
- Browser tab drag to wibar area (#318)
- Shadow geometry resize performance - excessive damage eliminated (#373)
- 6 naughty notification fixes: icon resolution (#343), ActionInvoked on dismiss (#344), timeout with `ruled.notification` (#345), GC ghost notifications (#346), `beautiful.notification_*` properties (#347), stuck notifications (#193)
- Hot-reload stability: Lgi FFI closure guard, GDBus singleton bypass, systray snapshot/restore, tiled client order preservation, stale titlebar/drawin cleanup
- Multi-monitor hotplug lifecycle (6 bugs including screen add ordering, layoutlist crash, scale reentrancy)
- Fullscreen clients now render above wibars (#368, #317)
- Client resize performance regression from shape updates (#359)
- Minimized clients no longer reappear after switching tags (#217)
- Lock screen covers all screens and survives hotplug (#353, #357)
- XWayland position sync for popup menu placement (#320)
- Firefox saved-geometry regression on map (#321)
- XWayland keyboard focus delivery in Lua focus path
- Various SEGV and use-after-free fixes in screen, spawn, drawin, and client lifecycle
- XKB multi-layout keyboard switching and widget display
- Pointer focus over titlebars, borders, and on client map
- Drag motion events delivered to drag source client
- Snap preview crash from format mismatch

### Changed

- 26-file AwesomeWM symbol alignment refactor for code-level parity
- 17 upstream AwesomeWM PRs ported (see `UPSTREAM_PORTS.md`)
- Dead xproperty stub functions and unused button matching code removed
- `selection()` crash converted to deprecation warning (use `selection.getter{}`)
- Build: strict GCC warnings enabled, LuaJIT preferred in auto-detection

### Notes

- AwesomeWM baseline: [`fa805ab4`](https://github.com/awesomeWM/awesome/commit/fa805ab465821c54094126b71a92acf2eba17674) (latest port: 2026-04-01)
- See [`DEVIATIONS.md`](DEVIATIONS.md) for all known Wayland vs X11 differences
- See [`UPSTREAM_PORTS.md`](UPSTREAM_PORTS.md) for ported AwesomeWM PRs

## [0.5.0] - 2026-01-02

### Breaking Changes

- **Build system migrated to meson** (https://github.com/trip-zip/somewm/discussions/117):
  - Now requires `meson` and `ninja` to build
  - wlroots 0.19 is bundled and built automatically (system wlroots 0.19 used if available)
  - `config.mk` removed - use `meson configure build` to change options
  - Old make targets removed: `install-local`, `install-session`, `uninstall-local`, `uninstall-session`
  - Build commands unchanged: `make` and `sudo make install` still work

- **CLI flags changed for AwesomeWM compatibility** (#4):
  - `-c` now specifies config file (was `-C`)
  - `-k` now runs config check (was `-c`)
  - If you were using `-C /path/to/config`, change to `-c /path/to/config`
  - If you were using `-c /path/to/config` for checking, change to `-k /path/to/config`

### Added

- ASAN/UBSAN build support via `make asan` for debugging memory issues
- Runtime cursor theme and size changing via `root.cursor_theme()` and `root.cursor_size()` (#177)
- Startup now respects `XCURSOR_THEME` and `XCURSOR_SIZE` environment variables (#177)

## [0.4.0] - 2025-12-28

### Added
- Dynamic keybinding removal (#15)
- Scroll wheel support in mousebinds (#16)
- Complete button press/release signals on clients (#17)
- Cursor shape changing via `root.cursor()` (#18)

### Notes
- XKB layout switching moved to 0.5.0 (Wayland limitation with documented workaround)

## [0.3.0] - 2025-12-21

Initial public release with core AwesomeWM compatibility.

[Unreleased]: https://github.com/trip-zip/somewm/compare/v1.4.5...HEAD
[1.4.5]: https://github.com/trip-zip/somewm/compare/v1.4.4...v1.4.5
[1.4.4]: https://github.com/trip-zip/somewm/compare/v1.4.3...v1.4.4
[1.4.3]: https://github.com/trip-zip/somewm/compare/v1.4.2...v1.4.3
[1.4.2]: https://github.com/trip-zip/somewm/compare/v1.4.1...v1.4.2
[1.4.1]: https://github.com/trip-zip/somewm/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/trip-zip/somewm/compare/0.5.0...v1.4.0
[0.5.0]: https://github.com/trip-zip/somewm/compare/0.4.0...0.5.0
[0.4.0]: https://github.com/trip-zip/somewm/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/trip-zip/somewm/releases/tag/0.3.0
