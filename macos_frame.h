/*
 * macos_frame.h - built-in macOS Big Sur window frame
 *
 * The compositor draws the whole window frame natively (C, via SceneFX): the
 * outer stroke, the inner highlight and the two-layer drop shadow are all
 * owned here, so a Lua config no longer needs to define or re-apply a frame.
 *
 * All frame values are compile-time constants below; they are Big Sur
 * approximations, not exact reproductions. There are no per-value theme
 * knobs. The only Lua surface is:
 *
 *   somewm.appearance      ("dark" | "light", global appearance mode)
 *   c.frame_appearance     ("dark" | "light", per-window appearance override)
 *   beautiful.macos_frame  (false disables the whole frame; default on)
 */
#ifndef SOMEWM_MACOS_FRAME_H
#define SOMEWM_MACOS_FRAME_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_shadow;
struct wlr_scene_tree;
typedef struct client_t client_t;

/* The per-client frame node state lives in objects/client.h (struct
 * macos_frame_nodes), so every translation unit that carries a client_t has a
 * complete type. Only macos_frame.c touches its internals. */

/* Corner radius shared by content, titlebars, stroke and highlight: the
 * frame clips everything it wraps to this single rounded rectangle. */
#define MACOS_FRAME_RADIUS 10

/* Outer stroke: 1 px OUTSIDE the window edge, following the radius.
 * Dark: black 50 %. Light: black 20 %. */
#define MACOS_FRAME_STROKE_WIDTH 1
#define MACOS_FRAME_STROKE_COLOR_DARK { 0.0f, 0.0f, 0.0f, 0.50f }
#define MACOS_FRAME_STROKE_COLOR_LIGHT { 0.0f, 0.0f, 0.0f, 0.20f }

/* Inner highlight: 1 px INSIDE the window edge, above client content and
 * titlebars, following the radius. DARK ONLY: white 16 % on the left, right
 * and bottom edges, top edge slightly brighter (white 22 %), blended smoothly
 * through the top corners. In light appearance no highlight node is created
 * at all (not just transparent). */
#define MACOS_FRAME_HIGHLIGHT_WIDTH 1
#define MACOS_FRAME_HIGHLIGHT_SIDE { 1.0f, 1.0f, 1.0f, 0.16f }
#define MACOS_FRAME_HIGHLIGHT_TOP { 1.0f, 1.0f, 1.0f, 0.22f }

/* Shadows (SceneFX shadow nodes), two layers per window. Focused windows
 * cast a larger, darker shadow than unfocused ones. The fork has no C tween
 * path for decoration properties (animation.c is a Lua callback ticker), so
 * the focused <-> unfocused switch is instant. */
#define MACOS_FRAME_SHADOW_FOCUSED_OFFSET_Y 22
#define MACOS_FRAME_SHADOW_FOCUSED_BLUR_1 70.0f
#define MACOS_FRAME_SHADOW_FOCUSED_COLOR_1 { 0.0f, 0.0f, 0.0f, 0.55f }
#define MACOS_FRAME_SHADOW_FOCUSED_BLUR_2 3.0f
#define MACOS_FRAME_SHADOW_FOCUSED_COLOR_2 { 0.0f, 0.0f, 0.0f, 0.45f }

#define MACOS_FRAME_SHADOW_UNFOCUSED_OFFSET_Y 12
#define MACOS_FRAME_SHADOW_UNFOCUSED_BLUR_1 40.0f
#define MACOS_FRAME_SHADOW_UNFOCUSED_COLOR_1 { 0.0f, 0.0f, 0.0f, 0.35f }
#define MACOS_FRAME_SHADOW_UNFOCUSED_BLUR_2 3.0f
#define MACOS_FRAME_SHADOW_UNFOCUSED_COLOR_2 { 0.0f, 0.0f, 0.0f, 0.35f }

/* ========== Global appearance mode ==========
 *
 * somewm.appearance is the single source of truth for the compositor-wide
 * appearance ("dark" by default when nothing sets it). Future effects must
 * query appearance_is_dark() here, never re-implement the state. */
bool appearance_is_dark(void);

/* ========== Frame API ========== */

/* beautiful.macos_frame (default on when unset). */
bool macos_frame_get_enabled(void);

/* Re-read beautiful.macos_frame at config load (alongside the shadow and
 * rounded defaults). */
void macos_frame_load_beautiful_defaults(lua_State *L);

/* Whether the macOS frame is drawn for this client right now. */
bool client_macos_frame_active(client_t *c);

/* Resolved appearance for a window: c->frame_appearance when set, else the
 * global mode. Returns 1 for dark, 0 for light. */
int client_macos_frame_appearance(client_t *c);

/* Effective corner radii the frame draws with (per-client corner_radius
 * override when set, else the Big Sur 10 px approximation). */
void client_macos_frame_radii(client_t *c, int radii[4]);

/* Rebuild the frame nodes for one client (cached; cheap when nothing
 * changed). Called every geometry pass. */
void client_macos_frame_update(client_t *c);

/* Visibility override used by the monitor-clamp pass in apply_geometry. */
void client_macos_frame_set_shown(client_t *c, bool shown);

/* Drop the frame's owned buffers and forget its scene nodes. Called when the
 * client's scene tree is being destroyed (the nodes die with the tree). */
void client_macos_frame_release(client_t *c);

/* Scale the frame decorations with a window opacity fade. */
void client_macos_frame_set_fade(client_t *c, float opacity);

/* Rebuild one client's frame immediately (c.frame_appearance writes). */
void client_macos_frame_repaint(client_t *c);

/* Re-apply the frame on every client (appearance or macos_frame switch). */
void macos_frame_reload_all(void);

/* Register the global `somewm` Lua object (somewm.appearance). */
void somewm_setup(lua_State *L);

#endif /* SOMEWM_MACOS_FRAME_H */