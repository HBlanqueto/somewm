#!/usr/bin/env bash
# Open/test the corner-enabled somewm build nested inside the current
# Wayland session (same as typing `somewm` in a terminal).
# Usage: ./test-nested.sh [start|stop|status|restart]
set -u
ROOT="$HOME/somewm"
BIN="$ROOT/build/somewm"
CLIENT="$ROOT/build/somewm-client"
LOG="/tmp/somewm-nested.log"
SOCKET="$HOME/.cache/somewm/interactive.socket"

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

start() {
    pgrep -f "$BIN$" >/dev/null && { echo "already running (pid $(pgrep -f "$BIN$"))"; return 1; }
    mkdir -p "$HOME/.cache/somewm"
    rm -f "$SOCKET"
    env \
        LUA_PATH="$HOME/.config/somewm/?.lua;$HOME/.config/somewm/?/init.lua;$ROOT/lua/?.lua;$ROOT/lua/?/init.lua;$LGI/share/lua/5.1/?.lua;$LGI/share/lua/5.1/?/init.lua" \
        LUA_CPATH="$LGI/lib/lua/5.1/?.so;;" \
        GI_TYPELIB_PATH="$GI_TYPELIB" \
        GDK_PIXBUF_MODULE_FILE="$PIXBUF_MODULES" \
        WLR_BACKENDS=wayland \
        WLR_RENDERER=pixman \
        WLR_WL_OUTPUTS=1 \
        TERMINAL=foot \
        NO_AT_BRIDGE=1 \
        SOMEWM_SOCKET="$SOCKET" \
        setsid bash -c "exec '$BIN'" >> "$LOG" 2>&1 &
    disown
    local i
    for i in $(seq 1 15); do sleep 1; [ -S "$SOCKET" ] && break; done
    if [ -S "$SOCKET" ]; then
        echo "started: pid $(pgrep -f "$BIN$") — window open on your screen"
        echo "socket:  $SOCKET"
        echo "log:     $LOG"
        run "awful = require('awful'); awful.spawn('foot -T somewm-test')" >/dev/null 2>&1 \
            && echo "opened a foot terminal inside it"
    else
        echo "FAILED to start — see $LOG"; tail -20 "$LOG"; return 1
    fi
}

stop() { pkill -f "$BIN$" && echo stopped || echo "not running"; }

status() {
    if pgrep -f "$BIN$" >/dev/null; then
        echo "running: pid $(pgrep -f "$BIN$")"
        [ -S "$SOCKET" ] && echo "socket:  $SOCKET"
    else
        echo "not running"
    fi
}

run() { env SOMEWM_SOCKET="$SOCKET" timeout 10 "$CLIENT" eval "$1" 2>&1 | tail -1; }

case "${1:-start}" in
    start)   start ;;
    stop)    stop ;;
    status)  status ;;
    restart) stop; sleep 1; start ;;
    run)     shift; run "$@" ;;
    *) echo "usage: $0 [start|stop|status|restart|run '<lua>']" >&2; exit 2 ;;
esac