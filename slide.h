/*
 * slide.h - macOS-style horizontal tag-switch slide
 *
 * When a tag switch goes from exactly one selected tag to exactly one other
 * tag on the same screen, the outgoing desktop slides out and the incoming
 * one slides in (scene-node moves only: never a configure, never a resize).
 * A solid vertical strip (the gap) travels between the two desktops and
 * everything revealed around them during the slide is black.
 *
 * The driver is C-only: each tick applies per-client visual_offset_x (the
 * same mechanism the focus-mode reveal uses for Y) and moves per-desktop
 * backdrop nodes. It is advanced from output frame events (rendermon) for
 * vsync-aligned motion, with some_refresh() as a time-based fallback.
 *
 * Focus-space tags (backdrop = black) get a persistent solid-black rect in
 * LyrBg whenever they are selected, independent of any animation, so the
 * black backdrop also holds when the slide is disabled or a switch is
 * instant.
 */

#ifndef SOMEWM_SLIDE_H
#define SOMEWM_SLIDE_H

#include <stdbool.h>

#include "objects/tag.h"
#include "somewm_types.h"

/* Per-tag backdrop kinds (Lua: tag.backdrop = "wallpaper" | "black"). */
enum tag_backdrop_kind {
    TAG_BACKDROP_WALLPAPER = 0,
    TAG_BACKDROP_BLACK,
};

void slide_init(void);
void slide_setup(lua_State *L);
void slide_hot_reload(lua_State *L);

/* Advance any active slide. Called from some_refresh() (fallback) and from
 * rendermon() on the sliding output (primary, vsync-aligned). */
void slide_tick(void);
/* Frame-event driver: called from rendermon() right before the scene commit.
 * Applies the frame's position so the presented frame carries it. */
void slide_tick_frame(void);

/* Called from banning_refresh(): keeps the persistent focus-space black rect
 * in sync, defers the real banning while a slide is running, and starts a
 * slide when a 1->1 tag switch is detected. Returns true when it took over
 * the switch (banning must not run). */
bool slide_handle_banning(void);

bool slide_active(void);
/* True when an active slide is running on monitor m (arrange() gates its
 * per-client visibility loop while true). */
bool slide_active_on(Monitor *m);

/* Persistent focus-space backdrop: show a black rect in LyrBg on m when any
 * selected tag there has backdrop == TAG_BACKDROP_BLACK. */
void slide_backdrop_refresh(Monitor *m);
void slide_backdrop_refresh_all(void);

/* Lua-facing configuration. */
void slide_set_enabled(bool enabled);
void slide_set_duration(double seconds);
void slide_set_easing(int easing);
void slide_set_gap(int px);
void slide_set_gap_color(const float rgba[4]);
/* Direct progress hook for a future touchpad gesture: p in [0,1]. When set,
 * the driver uses it instead of the clock. */
void slide_set_progress(double p);

#endif /* SOMEWM_SLIDE_H */