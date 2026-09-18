# Debugging SomeWM

SomeWM ships a built-in geometry tracer for the hardest class of bugs: a
window that ends up at the wrong size or position ("teleports") when you
focus it, click its tasklist entry, switch tags, or unminimize it. The tracer
records who moved the window — down to the C function and the Lua line.

## Enabling the tracer

Set `SOMEWM_TRACE` to any value when starting somewm:

```bash
SOMEWM_TRACE=1 somewm 2>/tmp/somewm-trace.log
```

It is read on first use and is off (and zero-cost) when unset. If your session
is started by a display manager, run somewm from a TTY as above, or read the
stderr your session logs it to.

`somewm -d` additionally turns on full wlroots debug logging. That is much
more verbose and independent of the geometry tracer.

## Geometry writes: `[TRACE-GEO]`

Every change to a client's geometry passes through one C funnel and is logged
as a one-line summary followed by two stacks:

```
[TRACE-GEO] +1234ms org.gnome.Nautilus 1084x638+0+0 -> 1084x638+0+24 (dx=0 dy=24 dw=0 dh=0) silent=0
[TRACE-GEO]   C frames (resolve with: addr2line -f -C -e <somewm-binary> <offset>):
[TRACE-GEO]   c#1 somewm(+0x6bc08) [0x...]
[TRACE-GEO]   c#2 somewm(+0x6ea8d) [0x...]
[TRACE-GEO]   lua#1 ./lua/awful/placement.lua:no_overlap:123
[TRACE-GEO]   lua#2 ./lua/awful/permissions/init.lua:activate:207
```

- `+Nms` is milliseconds since the first traced write, which makes it easy to
  correlate a write with the click that caused it.
- `silent=1` means the write took the silent path: the internal geometry is
  updated without emitting the `property::geometry` / `property::position` /
  `property::size` / `property::{x,y}` signals and without moving the client's
  screen. `silent=0` is the normal path that emits them.
- The C frames are libc backtrace offsets into the running (PIE) binary, so
  resolve them with the *same* binary:

  ```bash
  addr2line -f -C -e /path/to/somewm 0x6bc08
  ```

- The Lua lines are the live Lua call chain at the moment of the write (via
  `lua_getstack`), and are usually the quickest answer.

## Pointer presses: `[TRACE-BTN]`

To tie a write to the click that triggered it, every pointer press is marked:

```
[TRACE-BTN] drawin cursor=(1880,24) btn=272 state=1 client=nil focused_surface=(nil) mousegrabber=0
```

`<who>` is `drawin` (a wibox/wibar/titlebar), `client`, `layer` (a
layer-surface such as a panel), or `root`. `focused_surface` and `mousegrabber`
tell you whether the click was delivered to a client surface or intercepted by
a Lua mousegrabber.

## A typical session

```bash
SOMEWM_TRACE=1 somewm 2>/tmp/somewm-trace.log
# reproduce the problem (click the tasklist entry, switch tags, ...)
grep -aE 'TRACE-BTN|TRACE-GEO' /tmp/somewm-trace.log
```

The `[TRACE-BTN]` line for your click is immediately followed by any
`[TRACE-GEO]` write it caused, and the Lua stack names the culprit.

## Window-interaction notes

One behaviour is worth knowing before you blame a rule:

- **Fresh-client placement.** A newly mapped Wayland (XDG) toplevel is first
  seeded at its target monitor's workarea origin (so it never maps under a
  strut such as a top wibar), then the global `placement` rule in
  `somewmrc.lua` (`no_overlap + no_offscreen`) nudges it into free space. That
  first write is what a `+0+0`-looking seed corresponds to. XWayland clients
  carry their own position and are not seeded.

## See also

- `somewm.1` — `SOMEWM_TRACE` in the ENVIRONMENT section
- `docs/features_test.md` — rounded corners, shadows, hairline, autocolor
- `CONTRIBUTING.md` — building and running the test suite
