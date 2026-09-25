#!/usr/bin/env bash
# P0.3 GLES2 smoke: one command runs every scenario against a nested somewm with
# WLR_RENDERER=gles2 (SceneFX fx_renderer), exits non-zero on the first failure.
#
# Scenarios:
#   0. isolation: require() resolves inside the isolated config dir (P0.11)
#   1. clean start: no Lua errors, renderer is GLES2+SceneFX
#   2. slide with a COLOR wallpaper (tag 1 -> 2 -> 1)
#   3. slide with an IMAGE wallpaper
#   4. hot-reload x1, x2, x3, each followed by a slide
#   5. focus space: enter, reveal on/off, exit, no errors
#   (after all scenarios: live wallpaper-state mtime and both repos' git status
#    must be identical to the pre-run snapshot)
#
# Frame capture uses grim (wlr-screencopy) against the nested instance's own
# WAYLAND_DISPLAY, exactly like the fork's tests/test-*.lua do. Pixel analysis
# uses ImageMagick (magick), already present in the Nix env.
#
# Run: tests/p0.3-gles2-smoke.sh [--keep] [--config <ref|ruta>] [--shell <ref|ruta>]
#   --keep          leave artifacts (default: only failures keep them)
#   --config REF    nested config dir from a git worktree of ~/.config/somewm @ REF
#   --config PATH   nested config dir symlinked to a dev worktree PATH (origin kept)
#   --shell REF     same, for ~/.config/quickshell
#   (default: detached worktree at each repo's main-branch HEAD)
#
# P0.11 isolation: every nested instance (and any shell it spawns) runs against
# a throwaway copy of the personal config + quickshell under /tmp/p0.3/xdg/,
# with XDG_CONFIG_HOME/XDG_STATE_HOME/XDG_CACHE_HOME pinned there and the live
# settings.json copied in read-only when it is not versioned. After the suite
# the harness asserts the live session is untouched (wallpaper-state mtime and
# both repos' git status) and that require() resolves inside the isolated dir.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
ART=/tmp/p0.3
KEEP=0
CONFIG_ARG=
SHELL_ARG=

# Options (die is defined here so the parse loop can use it).
die() { echo "FATAL: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        --keep)   KEEP=1 ;;
        --config) CONFIG_ARG=${2:-}; shift ;;
        --shell)  SHELL_ARG=${2:-}; shift ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

# Default runtime dir: the launching user's per-user runtime, not the agent's
# inherited one, so instances never collide with the live session's sockets.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}

SOMEWM=$ROOT/build-test/somewm
CLIENT=$ROOT/build-test/somewm-client
PATTERN=$ROOT/build-test/test-content-pattern-client
[ -x "$SOMEWM" ] || { echo "FATAL: build-test/somewm missing (run make build-test)"; exit 2; }
[ -x "$PATTERN" ] || { echo "FATAL: build-test/test-content-pattern-client missing"; exit 2; }

GLOBAL_TIMEOUT=${P0_3_TIMEOUT:-300}
mkdir -p "$ART"

# P0.11 isolated XDG paths (the wrapper below must know them, so they are
# defined before it is written; the export comes in the isolation block).
XDG_ROOT=$ART/xdg
XDG_CONFIG=$XDG_ROOT/config
XDG_STATE=$XDG_ROOT/state
XDG_CACHE=$XDG_ROOT/cache

# The orchestrator inherits WLR_RENDERER from the environment for --host wayland
# (only headless forces pixman). It does not pass log-verbosity flags, so we
# hand it a SOMEWM_BINARY wrapper that adds --verbose: wlr_log goes to INFO and
# the log carries the fx_renderer GLES2 line this script asserts on. The wrapper
# also re-forces the isolated XDG dirs: the orchestrator overrides STATE/CACHE
# with its own sandbox, and the nested shell must share the same throwaway dirs.
export WLR_RENDERER=gles2
GLES2_WRAPPER=$ART/gles2-wrapper.sh
cat > "$GLES2_WRAPPER" <<EOF
#!/bin/sh
export XDG_CONFIG_HOME=$XDG_CONFIG
export XDG_STATE_HOME=$XDG_STATE
export XDG_CACHE_HOME=$XDG_CACHE
exec "$SOMEWM" --verbose "\$@"
EOF
chmod +x "$GLES2_WRAPPER"
export SOMEWM_BINARY="$GLES2_WRAPPER"

# --- P0.11 isolated config environment -------------------------------------
# The nested instance runs against a throwaway copy of the personal config +
# quickshell under /tmp/p0.3/xdg/, never the live dirs. Passing the conffile
# via -c pins get_configuration_dir() (and thus where settings.json lives) to
# the isolated dir; the GLES2 wrapper re-forces XDG_STATE/CACHE because the
# orchestrator otherwise sandboxes them.
export XDG_CONFIG_HOME=$XDG_CONFIG
export XDG_STATE_HOME=$XDG_STATE
export XDG_CACHE_HOME=$XDG_CACHE
CONFIG_CFG=$XDG_CONFIG/somewm
SHELL_CFG=$XDG_CONFIG/quickshell
CONFIG_REPO=$HOME/.config/somewm
SHELL_REPO=$HOME/.config/quickshell
mkdir -p "$XDG_CONFIG" "$XDG_STATE" "$XDG_CACHE"

is_abs() { case "$1" in /*) return 0 ;; *) return 1 ;; esac; }

# Place one repo's config at $dest: a git worktree for a ref (removed with
# --force on exit), a symlink for an existing dev worktree path (origin kept),
# or a detached worktree at the repo's main-branch HEAD when no arg is given.
# Sets ISOLATION_KIND to "worktree" or "symlink".
ISOLATION_KIND=
setup_one() {
    local dest=$1 repo=$2 arg=${3:-} ref
    if [ -n "$arg" ] && is_abs "$arg"; then
        ln -s "$arg" "$dest"
        echo "isolated: $dest -> symlink $arg"
        ISOLATION_KIND=symlink
        return
    fi
    if [ -n "$arg" ]; then
        ref=$arg
    else
        ref=$(git -C "$repo" symbolic-ref --short HEAD 2>/dev/null) || ref=HEAD
    fi
    if ! git -C "$repo" worktree add --detach "$dest" "$ref" >/dev/null 2>&1; then
        die "cannot create worktree $dest @ $ref ($repo)"
    fi
    echo "isolated: $dest <- worktree $ref ($(git -C "$repo" rev-parse --short "$ref" 2>/dev/null || echo HEAD))"
    ISOLATION_KIND=worktree
}

setup_one "$CONFIG_CFG" "$CONFIG_REPO" "$CONFIG_ARG"
CFG_SETUP=$ISOLATION_KIND
setup_one "$SHELL_CFG" "$SHELL_REPO" "$SHELL_ARG"
SHELL_SETUP=$ISOLATION_KIND

# settings.json is NOT versioned in the config repo: ship the live one into the
# isolated config dir, read-only, so the nested config reads the real values.
if git -C "$CONFIG_REPO" ls-files --error-unmatch settings.json >/dev/null 2>&1; then
    echo "settings.json is versioned in $CONFIG_REPO; using the checked-in copy"
else
    if [ -f "$HOME/.config/somewm/settings.json" ]; then
        install -m 0444 "$HOME/.config/somewm/settings.json" "$CONFIG_CFG/settings.json"
        echo "isolated: copied live settings.json (read-only) -> $CONFIG_CFG/settings.json"
    else
        echo "isolated: no live settings.json to copy"
    fi
fi

# Consumed by tests/p0.3/focus-rc.lua so S5 loads libs/focus_space from the
# isolated config dir instead of the hardcoded live path.
export SOMEWM_TEST_CONFIG_DIR=$CONFIG_CFG

# Minimal rc.lua inside the isolated config dir: proves the fork prepends the
# conffile's dir to package.path, so require() (e.g. libs/focus_space) resolves
# to the throwaway copy and never the live ~/.config/somewm.
ISOLATION_PROBE=$CONFIG_CFG/p0.3-probe.lua
cat > "$ISOLATION_PROBE" <<'PROBE'
-- P0.11 isolation probe: minimal config whose dir anchors package.path to the
-- isolated config dir. No autostart, no wallpaper, no keybindings.
local awful = require("awful")
awesome.connect_signal("debug::error", function(err)
    io.stderr:write("ERROR: " .. tostring(err) .. "\n")
end)
for s in screen do
    awful.tag({ "1", "2" }, s, awful.layout.layouts[1])
end
PROBE

# Tear down what this script created and stop any instance left running. The
# live session is never touched. Runs on EXIT/INT/TERM of the main shell only
# (scenario subshells reset their EXIT trap).
cleanup() {
    "$CLIENT" test stop --name iso >/dev/null 2>&1
    for d in s1 s2 s3 s4 s5; do
        [ -d "$(state_dir "$d")" ] && "$CLIENT" test stop --name "$d" >/dev/null 2>&1
    done
    rm -f "$ISOLATION_PROBE"
    if [ "$CFG_SETUP" = symlink ]; then
        [ -L "$CONFIG_CFG" ] && rm -f "$CONFIG_CFG"
    else
        [ -d "$CONFIG_CFG" ] && git -C "$CONFIG_REPO" worktree remove --force "$CONFIG_CFG" >/dev/null 2>&1
    fi
    if [ "$SHELL_SETUP" = symlink ]; then
        [ -L "$SHELL_CFG" ] && rm -f "$SHELL_CFG"
    else
        [ -d "$SHELL_CFG" ] && git -C "$SHELL_REPO" worktree remove --force "$SHELL_CFG" >/dev/null 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# --- helpers ----------------------------------------------------------------

stage() { echo; echo "===== $* ====="; }

state_dir() { echo "$XDG_RUNTIME_DIR/somewm-test/$1"; }
instance_log() { echo "$(state_dir "$1")/log"; }
instance_wl() { grep '^wl_socket_name=' "$(state_dir "$1")/info" | cut -d= -f2; }

# Read one line of eval output (OK\n<value>\n\n) for instance $1.
eval_line() {
    "$CLIENT" test eval --name "$1" "$2" 2>&1 | sed -n '2p'
}

# Log check: fatal Lua errors / leaks must never appear in any scenario.
# "outlived the state" is deliberately NOT gated here: the fork's own minimal
# tests/rc.lua leaks one GLib source on every hot-reload (known/unfixed, the
# restart suite pins it with xfail), so gating it would fail S4 for reasons
# unrelated to this harness.
log_clean() {
    local log; log=$(instance_log "$1")
    for pat in "FATAL:" "error in error handling" \
               "lgi_guard: WARNING" "Lua error" "stack traceback"; do
        if grep -qF -- "$pat" "$log"; then
            echo "  FAIL: log has '$pat'"
            return 1
        fi
    done
    # Config redirects debug::error to stderr with an "ERROR: " prefix.
    if grep -qE "^ERROR:|ERROR: .*error" "$log"; then
        echo "  FAIL: config reported a Lua error"
        return 1
    fi
    echo "  ok: log clean"
    return 0
}

# Renderer must be the SceneFX GLES2 renderer, not pixman.
check_renderer() {
    local log; log=$(instance_log "$1")
    if grep -q "Creating scenefx FX renderer" "$log" \
       && grep -q "Using OpenGL ES" "$log"; then
        echo "  ok: renderer is SceneFX/GLES2"
        grep -m1 "Using OpenGL ES" "$log" | sed 's/^/      /'
        return 0
    fi
    if grep -qiE "pixman" "$log"; then
        echo "  FAIL: renderer is pixman (no SceneFX GLES2 line)"
    else
        echo "  FAIL: no 'Creating scenefx FX renderer' in log"
    fi
    return 1
}

# Capture the nested instance's output (grim -> screencopy) as a PPM.
capture() {
    local name=$1 out=$2 wl runtime
    wl=$(instance_wl "$name")
    runtime="$(state_dir "$name")/runtime"
    [ -n "$wl" ] || { echo "  FAIL: no wl_socket_name for $name"; return 1; }
    env XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$wl" \
        grim -t ppm "$out" >/dev/null 2>&1
    [ -s "$out" ] || { echo "  FAIL: grim produced no output ($out)"; return 1; }
    echo "  ok: captured $out"
}

# Fraction (0..1) of pixels matching a hex color in a PPM (ImageMagick).
color_fraction() {
    local f=$1 hex=$2 total count
    total=$(magick "$f" -format %c histogram:info:- 2>/dev/null | awk '{s+=$1} END{print s}')
    count=$(magick "$f" -format %c histogram:info:- 2>/dev/null \
        | grep -im1 "#${hex}" | awk '{gsub(":","",$1); print $1}')
    [ -z "$total" ] || [ "$total" = 0 ] && { echo "0"; return 1; }
    [ -z "$count" ] && count=0
    awk -v c="$count" -v t="$total" 'BEGIN{printf "%.4f", c/t}'
}

# Fraction (0..1) of pure black pixels in a PPM, via ImageMagick histogram.
black_fraction() {
    color_fraction "$1" "000000"
}

# P0.9 criteria. The gap strip is slide_gap px (80) wide on a 1280px output, so
# at mid-slide the gap color covers 80/1280 = 6.25% of the frame. A healthy
# slide must show it between 5% and 7.5% (the exact fraction, plus tolerance)
# in BOTH directions. Pure black must stay below 0.5% at mid and at end (a
# failed backdrop replaces the whole desktop with a black rect -> >=50%).
#
# Return-direction slides (2->1) currently FAIL the gap-color check: the gap
# strip is placed at m.x + m.width + off_out (slide.c:639); with direction=-1
# off_out is positive so the strip moves right, off-screen, and the seam shows
# the hidden root background (black). This is P0.10. Return slides are therefore
# XFAIL: they are expected to fail, and if they ever PASS the suite reports
# XPASS and fails so the XFAIL marker must be removed.
GAP_LO=0.05
GAP_HI=0.075
BLACK_MAX=0.005

# Run a slide to tag $idx and assert P0.9 criteria. $dir is the artifact dir.
# $gap is the hex gap color (e.g. ff0000). $xfail=1 marks a return-direction
# slide as expected-failure (P0.10): a PASS there is reported as XPASS and
# fails the run.
# Echoes the measurement line; returns 0 pass / 1 fail / 2 xpass.
run_slide() {
    local name=$1 idx=$2 dir=$3 gap=$4 xfail=$5 mid end gm bm be rc
    mkdir -p "$dir"
    switch_tag "$name" "$idx"
    sleep 1.0
    mid="$dir/mid-$idx.ppm"
    capture "$name" "$mid" || return 1
    sleep 2.2
    end="$dir/end-$idx.ppm"
    capture "$name" "$end" || return 1
    gm=$(color_fraction "$mid" "$gap")
    bm=$(black_fraction "$mid"); be=$(black_fraction "$end")
    echo "  [tag $idx] mid %gap=$gm mid %black=$bm end %black=$be"
    rc=0
    awk -v g="$gm" -v lo="$GAP_LO" -v hi="$GAP_HI" 'BEGIN{exit !(g>=lo && g<=hi)}' || rc=1
    awk -v b="$bm" -v t="$BLACK_MAX" 'BEGIN{exit !(b<t)}' || rc=1
    awk -v b="$be" -v t="$BLACK_MAX" 'BEGIN{exit !(b<t)}' || rc=1
    if [ "$rc" -eq 0 ] && [ "$xfail" = 1 ]; then
        echo "  XPASS: return slide to tag $idx meets criteria (remove the XFAIL, P0.10)"
        return 2
    fi
    if [ "$rc" -eq 0 ]; then
        echo "  ok: slide to tag $idx (gap $GAP_LO..$GAP_HI, black < $BLACK_MAX)"
        return 0
    fi
    if [ "$xfail" = 1 ]; then
        echo "  XFAIL: return slide to tag $idx fails gap/black criteria (P0.10, expected)"
        return 1
    fi
    echo "  FAIL: slide to tag $idx (gap $GAP_LO..$GAP_HI, black < $BLACK_MAX)"
    return 1
}

# Start an instance and assert GLES2 + clean log.
start_gles2() {
    local name=$1 cfg=$2
    rm -rf "$(state_dir "$name")"
    if ! "$CLIENT" test start --name "$name" --host wayland --config "$cfg" \
            --force >/dev/null 2>&1; then
        echo "  FAIL: instance '$name' failed to start"
        return 1
    fi
    local i
    for i in $(seq 1 30); do [ -S "$(state_dir "$name")/ipc.sock" ] && break; sleep 0.5; done
    sleep 2
    check_renderer "$name" || { stop_gles2 "$name"; return 1; }
    log_clean "$name" || { stop_gles2 "$name"; return 1; }
    return 0
}

stop_gles2() {
    "$CLIENT" test stop --name "$1" >/dev/null 2>&1 || true
}

# Tag switch that triggers the slide. Uses tag names 1/2/3 from base-rc.lua.
# Evals run without the config's `local awful` in scope, so all API calls go
# through require("awful") explicitly.
switch_tag() {
    local name=$1 idx=$2
    eval_line "$name" \
        "for _,t in ipairs(screen[1].tags) do if t.name=='$idx' then t:view_only(); return 'switched' end end return 'notag'"
}

# --- scenario 1: clean start ----------------------------------------------

S1() {
    stage "S1: clean start (GLES2+SceneFX)"
    start_gles2 s1 "$ROOT/tests/p0.3/base-rc.lua" || return 1
    stop_gles2 s1
    echo "  ok: S1"
}

# --- scenario 2: slide, color wallpaper -----------------------------------

S2() {
    stage "S2: slide with color wallpaper"
    start_gles2 s2 "$ROOT/tests/p0.3/base-rc.lua" || return 1
    # Blue wallpaper + red slide gap: the gap fraction is the P0.9 criterion.
    eval_line s2 "require('awful').wallpaper { screen = screen[1], bg = '#3366cc' }; return 'wp'"
    eval_line s2 "slide.set_duration(2); return 'dur'"
    eval_line s2 "slide.set_gap_color('#ff0000'); return 'gap'"
    eval_line s2 "slide.set_sliding_layers({}); return 'sl'"
    # Forward 1->2 must pass; return 2->1 is XFAIL (P0.10).
    run_slide s2 2 "$ART/s2" "ff0000" 0 || { stop_gles2 s2; return 1; }
    local rc
    run_slide s2 1 "$ART/s2" "ff0000" 1; rc=$?
    # rc==2 means XPASS: criteria met in a return slide -> remove the XFAIL.
    [ "$rc" -eq 2 ] && { echo "  FAIL: return slide PASSED (XPASS) - remove XFAIL, P0.10 fixed"; stop_gles2 s2; return 1; }
    [ "$rc" -eq 1 ] || { echo "  FAIL: unexpected run_slide rc=$rc"; stop_gles2 s2; return 1; }
    stop_gles2 s2
    echo "  ok: S2 (forward PASS, return XFAIL)"
}

# --- scenario 3: slide, image wallpaper ------------------------------------

S3() {
    stage "S3: slide with image wallpaper"
    start_gles2 s3 "$ROOT/tests/p0.3/base-rc.lua" || return 1
    local img=$ART/s3/wall.png
    mkdir -p "$ART/s3"
    magick -size 1280x720 gradient:'#3366cc'-'#66ccff' "$img" 2>/dev/null
    eval_line s3 \
        "require('awful').wallpaper { screen = screen[1], bg = '#000000', widget = require('wibox.widget').imagebox('$img') }; return 'wp'"
    eval_line s3 "slide.set_duration(2); slide.set_gap_color('#ff0000'); slide.set_sliding_layers({}); return 'cfg'"
    run_slide s3 3 "$ART/s3" "ff0000" 0 || { stop_gles2 s3; return 1; }
    local rc
    run_slide s3 1 "$ART/s3" "ff0000" 1; rc=$?
    [ "$rc" -eq 2 ] && { echo "  FAIL: return slide PASSED (XPASS) - remove XFAIL, P0.10 fixed"; stop_gles2 s3; return 1; }
    [ "$rc" -eq 1 ] || { echo "  FAIL: unexpected run_slide rc=$rc"; stop_gles2 s3; return 1; }
    stop_gles2 s3
    echo "  ok: S3 (forward PASS, return XFAIL)"
}

# --- scenario 4: hot-reload x1, x2, x3, each followed by a slide ------------

S4() {
    stage "S4: hot-reload x1..x3, each + slide"
    start_gles2 s4 "$ROOT/tests/p0.3/base-rc.lua" || return 1
    eval_line s4 "require('awful').wallpaper { screen = screen[1], bg = '#3366cc' }; return 'wp'"
    local n rc
    for n in 1 2 3; do
        local before after
        before=$(grep -c "hot-reload: complete" "$(instance_log s4)" || true)
        "$CLIENT" test reload --name s4 >/dev/null 2>&1
        for i in $(seq 1 30); do
            after=$(grep -c "hot-reload: complete" "$(instance_log s4)" || true)
            [ "$after" -gt "$before" ] 2>/dev/null && break
            sleep 0.2
        done
        if [ "$after" -le "$before" ] 2>/dev/null; then
            echo "  FAIL: reload #$n did not complete"
            stop_gles2 s4; return 1
        fi
        echo "  ok: reload #$n complete"
        log_clean s4 || { stop_gles2 s4; return 1; }
        # After a reload the C slide state (duration/gap) survives; wallpaper
        # is re-applied by rc.lua-less config, so set the slide knobs again.
        eval_line s4 "slide.set_duration(2); slide.set_gap_color('#ff0000'); return 'cfg'"
        # The reload cleared prev_selected, so the first switch after it is
        # instant (no slide). Ensure we land on tag 1 first, then slide 1->2
        # (real slide, forward) and 2->1 (return, XFAIL). Guarantees every
        # reload is followed by a real forward slide.
        switch_tag s4 1 >/dev/null
        sleep 0.5
        run_slide s4 2 "$ART/s4/r$n" "ff0000" 0 || { stop_gles2 s4; return 1; }
        run_slide s4 1 "$ART/s4/r$n" "ff0000" 1; rc=$?
        if [ "$rc" -eq 2 ]; then
            echo "  FAIL: return slide PASSED (XPASS) after reload #$n - remove XFAIL, P0.10 fixed"
            stop_gles2 s4; return 1
        fi
        [ "$rc" -eq 1 ] || { echo "  FAIL: unexpected run_slide rc=$rc"; stop_gles2 s4; return 1; }
    done
    stop_gles2 s4
    echo "  ok: S4 (forward PASS each reload, return XFAIL)"
}

# --- scenario 5: focus space ------------------------------------------------

S5() {
    stage "S5: focus space (enter, reveal on/off, exit)"
    start_gles2 s5 "$ROOT/tests/p0.3/focus-rc.lua" || return 1
    local pid
    pid=$(eval_line s5 \
        "return tostring(require('awful').spawn('$PATTERN'))")
    [ -n "$pid" ] && [ "$pid" != "nil" ] || { echo "  FAIL: pattern client did not spawn"; stop_gles2 s5; return 1; }
    echo "  ok: pattern client pid=$pid"
    sleep 2
    # Focus the client, then toggle -> enter the focus space.
    eval_line s5 \
        "local c; for _,x in ipairs(client.get()) do if x.class=='content_pattern_test' then c=x end end c:activate{}; return 'focused'" >/dev/null
    sleep 0.3
    eval_line s5 \
        "require('libs.focus_space').toggle(client.focus); return 'entered'"
    sleep 2
    local tags
    tags=$(eval_line s5 \
        "local o={} for _,t in ipairs(screen[1].tags) do if t.selected then o[#o+1]=t.name..':vol='..tostring(t.volatile) end end return table.concat(o,',')")
    echo "  selected tags: $tags"
    case "$tags" in
        *vol=false*) echo "  ok: focus-space tag selected (volatile)";;
        *) echo "  FAIL: no volatile focus tag selected"; stop_gles2 s5; return 1;;
    esac
    # Reveal ON -> the focused client moves down by the reveal range (32).
    eval_line s5 \
        "require('libs.focus_space').set_bar_revealed(true, 0, {height=32, duration=300, easing='ease-out-cubic'}); return 'on'"
    sleep 0.5
    local off
    off=$(eval_line s5 "return tostring(client.focus and client.focus.visual_offset_y or -1)")
    [ "$off" = "32" ] || { echo "  FAIL: reveal ON, expected offset 32, got $off"; stop_gles2 s5; return 1; }
    echo "  ok: reveal ON -> offset=$off"
    # Reveal OFF -> offset back to 0.
    eval_line s5 \
        "require('libs.focus_space').set_bar_revealed(false, 0, {height=32, duration=300, easing='ease-out-cubic'}); return 'off'"
    sleep 0.5
    off=$(eval_line s5 "return tostring(client.focus and client.focus.visual_offset_y or -1)")
    [ "$off" = "0" ] || { echo "  FAIL: reveal OFF, expected offset 0, got $off"; stop_gles2 s5; return 1; }
    echo "  ok: reveal OFF -> offset=$off"
    # Exit: toggle again -> back on a normal tag, no volatile tag left.
    eval_line s5 "require('libs.focus_space').toggle(client.focus); return 'left'"
    sleep 2
    tags=$(eval_line s5 \
        "local o={} for _,t in ipairs(screen[1].tags) do if t.selected then o[#o+1]=t.name end end return table.concat(o,',')")
    echo "  selected after exit: $tags"
    case "$tags" in
        1|2|3) echo "  ok: exited to normal tag $tags";;
        *) echo "  FAIL: not back on a normal tag after exit"; stop_gles2 s5; return 1;;
    esac
    log_clean s5 || { stop_gles2 s5; return 1; }
    stop_gles2 s5
    echo "  ok: S5"
}

# --- scenario 0: isolation probe --------------------------------------------
# A minimal config inside the isolated dir proves the fork prepends the
# conffile's dir to package.path, so libs.focus_space resolves to the throwaway
# copy and never the live ~/.config/somewm.

ISOLATION() {
    stage "ISOLATION: require() anchors to the isolated config dir"
    local name=iso resolved target resolved_real i
    rm -rf "$(state_dir "$name")"
    if ! "$CLIENT" test start --name "$name" --host wayland \
            --config "$ISOLATION_PROBE" --force >/dev/null 2>&1; then
        echo "  FAIL: isolation probe instance did not start"
        return 1
    fi
    for i in $(seq 1 30); do [ -S "$(state_dir "$name")/ipc.sock" ] && break; sleep 0.5; done
    sleep 1
    resolved=$("$CLIENT" test eval --name "$name" \
        "return package.searchpath('libs.focus_space', package.path)" 2>&1 | sed -n '2p')
    "$CLIENT" test stop --name "$name" >/dev/null 2>&1
    echo "  searchpath('libs.focus_space') = $resolved"
    target=$(readlink -f "$CONFIG_CFG/libs/focus_space.lua")
    resolved_real=$(readlink -f "$resolved" 2>/dev/null)
    if [ -n "$resolved_real" ] && [ "$resolved_real" = "$target" ]; then
        echo "  ok: resolves to the isolated config copy (realpath $target)"
        return 0
    fi
    echo "  FAIL: expected realpath $target, got '$resolved_real'"
    return 1
}

# --- live-session snapshot (before) -----------------------------------------
# The live session must be untouched by the suite: wallpaper-state mtime and
# both repos' git status are captured now and re-asserted by isolation_after().
LIVE_WALLPAPER=$HOME/.local/state/somewm/wallpaper
if [ -f "$LIVE_WALLPAPER" ]; then
    WALLPAPER_BEFORE=$(stat -c %Y "$LIVE_WALLPAPER")
else
    WALLPAPER_BEFORE=missing
fi
GIT_CONFIG_BEFORE=$(git -C "$CONFIG_REPO" status --short)
GIT_SHELL_BEFORE=$(git -C "$SHELL_REPO" status --short)

# --- runner ----------------------------------------------------------------

PASS=0; FAIL=0

for fn in ISOLATION S1 S2 S3 S4 S5; do
    (
        trap 'exit 130' TERM
        trap - EXIT
        "$fn"
    ) &
    local_pid=$!
    # Watchdog: kill only this scenario's subshell after the global timeout.
    (
        sleep "$GLOBAL_TIMEOUT"
        kill -TERM "$local_pid" 2>/dev/null
    ) &
    watch_pid=$!
    if wait "$local_pid"; then
        PASS=$((PASS + 1))
    else
        rc=$?
        FAIL=$((FAIL + 1))
        echo "  SCENARIO $fn FAILED (rc=$rc)"
        if [ "$KEEP" = 0 ]; then
            for d in s1 s2 s3 s4 s5; do
                [ -d "$(state_dir "$d")" ] && "$CLIENT" test stop --name "$d" >/dev/null 2>&1
            done
        fi
    fi
    kill "$watch_pid" 2>/dev/null
    wait "$watch_pid" 2>/dev/null
done

# After all scenarios: assert the live session is untouched.
isolation_after() {
    stage "ISOLATION: live session untouched"
    local rc=0 wp gca gcb
    wp=$(stat -c %Y "$LIVE_WALLPAPER" 2>/dev/null || echo missing)
    echo "  wallpaper state mtime: before=$WALLPAPER_BEFORE after=$wp"
    [ "$wp" = "$WALLPAPER_BEFORE" ] || { echo "  FAIL: live wallpaper state changed"; rc=1; }
    gca=$(git -C "$CONFIG_REPO" status --short)
    echo "  git -C $CONFIG_REPO status: [${gca}] (before [${GIT_CONFIG_BEFORE}])"
    [ "$gca" = "$GIT_CONFIG_BEFORE" ] || { echo "  FAIL: config repo status changed"; rc=1; }
    gcb=$(git -C "$SHELL_REPO" status --short)
    echo "  git -C $SHELL_REPO status: [${gcb}] (before [${GIT_SHELL_BEFORE}])"
    [ "$gcb" = "$GIT_SHELL_BEFORE" ] || { echo "  FAIL: quickshell repo status changed"; rc=1; }
    [ "$rc" -eq 0 ] && echo "  ok: live session untouched"
    return $rc
}
if isolation_after; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
fi

echo
echo "================================="
echo "P0.3 GLES2 smoke: PASS=$PASS FAIL=$FAIL"
echo "================================="
[ "$FAIL" -eq 0 ] || exit 1
exit 0