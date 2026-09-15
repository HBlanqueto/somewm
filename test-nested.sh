#!/usr/bin/env bash
# Open somewm nested inside the current Wayland session (same as running
# `somewm` in a terminal).
#
#   ./test-nested.sh              start with the repo's default config (roundy)
#   ./test-nested.sh user         start with your own ~/.config/somewm config
#   ./test-nested.sh status       show whether it is running
#   ./test-nested.sh stop         stop it
#   ./test-nested.sh restart      stop + start (same config mode as last start)
#   ./test-nested.sh run '<lua>'  evaluate one IPC command against it
#
# "default" mode uses THIS folder's somewmrc.lua with the roundy theme
# (rounded corners + follow-corner shadows) instead of your personal
# ~/.config/somewm/rc.lua. It also prefers the build-test/somewm binary,
# which includes shadow_follow_corners; falls back to build/somewm if absent.
#
# The Luajit/LGI runtime MUST be somewm's dev-shell closure (fixed below).
# Using a generic `nix-shell -p luajit luajitPackages.lgi` loads a SECOND
# GLib/GdkPixbuf/Rsvg/Pango closure into the process and deadlocks on client
# map ("RsvgHandle already registered", dual-glib type clashes). The paths
# below are exactly the closure build-test/somewm is linked against.
set -u
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

BIN="${SOMEWM_BIN:-}"
if [ -z "$BIN" ]; then
    BIN="$ROOT/build-test/somewm"   # current build with shadow_follow_corners
    [ -x "$BIN" ] || BIN="$ROOT/build/somewm"
fi
CLIENT="$ROOT/build/somewm-client"
LOG="/tmp/somewm-nested.log"
SOCKET="$HOME/.cache/somewm/interactive.socket"
THEME="${SOMEWM_THEME:-roundy}"

# --- Luajit/LGI runtime paths (fixed to somewm's dev-shell closure) ---------
LGI="/nix/store/23fzxml1aqk5r8k42fwai5rpvanwn77p-luajit-2.1.1774638290-env"
GI_TYPELIB="/nix/store/qjhvmcbysvdvvrxc26bgz0b28jhikqs5-gdk-pixbuf-2.44.6/lib/girepository-1.0\
:/nix/store/vx2aqxs99ii6pkzndddnbrsqbc4jwpb6-glib-2.88.1/lib/girepository-1.0\
:/nix/store/cgrfnkpigc38vq5hc067n5pf0svd0xnx-gobject-introspection-wrapped-1.86.0/lib/girepository-1.0\
:/nix/store/sqnii8dl55xsjyyp11z8yj8kn4m98arv-harfbuzz-13.2.1/lib/girepository-1.0\
:/nix/store/asams9pxmvyf0b1z604rckjlcwsqfjsz-librsvg-2.62.3/lib/girepository-1.0\
:/nix/store/5x3899zr68zkavkq8lcl539ny9mzr1ml-pango-1.57.1/lib/girepository-1.0\
:/nix/store/2s390ny0h0mk176dmqhy7x41rw6crlb0-gtk+3-3.24.52/lib/girepository-1.0"
# gdk-pixbuf svg loader must come from the SAME librsvg closure as lgi's Rsvg,
# otherwise a second librsvg instance aborts with "RsvgHandle already registered".
PIXBUF_MODULES="/nix/store/asams9pxmvyf0b1z604rckjlcwsqfjsz-librsvg-2.62.3/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"

# Since last "start" used (persisted in the state dir itself).
STATEDIR="$HOME/.cache/somewm"
DEFAULT_CONFIG_HOME="$STATEDIR/default-config"   # isolated XDG_CONFIG_HOME for default mode

start() {
    local mode="${1:-default}"
    if pgrep -f "^$BIN" >/dev/null; then
        echo "already running (pid $(pgrep -f "^$BIN" | head -1))"
        return 1
    fi
    mkdir -p "$STATEDIR"
    rm -f "$SOCKET"

    local extra=()
    if [ "$mode" = "default" ]; then
        # Isolated XDG_CONFIG_HOME so the repo config's theme picker
        # (somewmrc.lua theme file) never touches ~/.config/somewm.
        mkdir -p "$DEFAULT_CONFIG_HOME/somewm"
        printf '%s\n' "$THEME" > "$DEFAULT_CONFIG_HOME/somewm/theme"
        # themes resolved from this repo (roundy lives here)
        extra=( AWESOME_THEMES_PATH="$ROOT/themes" XDG_CONFIG_HOME="$DEFAULT_CONFIG_HOME" )
        CONFIG_FLAG=(-c "$ROOT/somewmrc.lua")
    else
        CONFIG_FLAG=()
    fi
    printf '%s\n' "$mode" > "$STATEDIR/nested-mode"

    env \
        LUA_PATH="$HOME/.config/somewm/?.lua;$HOME/.config/somewm/?/init.lua;$ROOT/lua/?.lua;$ROOT/lua/?/init.lua;$LGI/share/lua/5.1/?.lua;$LGI/share/lua/5.1/?/init.lua" \
        LUA_CPATH="$LGI/lib/lua/5.1/?.so;;" \
        GI_TYPELIB_PATH="$GI_TYPELIB" \
        GDK_PIXBUF_MODULE_FILE="$PIXBUF_MODULES" \
        "${extra[@]}" \
        WLR_BACKENDS=wayland \
        WLR_RENDERER=pixman \
        WLR_WL_OUTPUTS=1 \
        TERMINAL=foot \
        NO_AT_BRIDGE=1 \
        SOMEWM_SOCKET="$SOCKET" \
        setsid bash -c "exec '$BIN' ${CONFIG_FLAG[*]:-}" >> "$LOG" 2>&1 &
    disown

    local i
    for i in $(seq 1 15); do sleep 1; [ -S "$SOCKET" ] && break; done
    if [ -S "$SOCKET" ]; then
        echo "started: pid $(pgrep -f "^$BIN" | head -1) — window open on your screen"
        echo "socket:  $SOCKET"
        echo "log:     $LOG"
        [ "$mode" = "default" ] && echo "config:  $ROOT/somewmrc.lua (theme: $THEME)"
        run "awful = require('awful'); awful.spawn('foot -T somewm-test')" >/dev/null 2>&1 \
            && echo "opened a foot terminal inside it"
    else
        echo "FAILED to start — see $LOG"; tail -20 "$LOG"; return 1
    fi
}

stop() { pkill -f "^$BIN" && echo stopped || echo "not running"; }

status() {
    if pgrep -f "^$BIN" >/dev/null; then
        echo "running: pid $(pgrep -f "^$BIN" | head -1)"
        [ -S "$SOCKET" ] && echo "socket:  $SOCKET"
    else
        echo "not running"
    fi
}

run() { env SOMEWM_SOCKET="$SOCKET" timeout 10 "$CLIENT" eval "$1" 2>&1 | grep -v '^$' | tail -1; }

mode_of_last() { [ -f "$STATEDIR/nested-mode" ] && cat "$STATEDIR/nested-mode" || echo default; }

case "${1:-start}" in
    start)     start "${2:-default}" ;;
    user)      start user ;;
    default)   start default ;;
    stop)      stop ;;
    status)    status ;;
    restart)   stop; sleep 1; start "$(mode_of_last)" ;;
    run)       shift; run "$@" ;;
    *) echo "usage: $0 [start [default|user]|user|default|stop|status|restart|run '<lua>']" >&2; exit 2 ;;
esac