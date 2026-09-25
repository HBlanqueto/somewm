/*
 * macos_frame.c - built-in macOS Big Sur window frame (SceneFX)
 *
 * Draws the whole decorated window frame natively in C: the outer stroke, the
 * inner highlight and the single drop shadow, plus the compositor-wide
 * appearance mode (somewm.appearance) that drives them. The Lua config no
 * longer defines or re-applies a frame; everything is a compile-time constant
 * (macos_frame.h) except the two documented mode knobs (somewm.appearance,
 * c.frame_appearance).
 *
 * Geometry (macOS): with W = the window rect (titlebar + content, the area
 * the client occupies) and R = 10 px corner radius, from outside to inside:
 *
 *   1. Shadow: one SceneFX shadow node behind everything, derived from W's
 *      rounded shape (radius R), offset down, clipped to W so translucent
 *      windows never let it bleed through.
 *   2. Outer stroke: 1 px ring OUTSIDE W - outer edge = W grown by 1 px
 *      (radius R+1), inner edge = W exactly (radius R). Below the client,
 *      above the shadow. Dark: black 50 %. Light: black 20 %.
 *   3. Content + titlebars clipped to W with radius R.
 *   4. Inner highlight: 1 px ring INSIDE W, above the titlebar buffers and
 *      client surface, below popups. Dark only: white 16 % sides/bottom,
 *      white 22 % top, blended through the top corners.
 *
 * The frame nodes are children of c->scene, whose origin is the OUTER border
 * corner of the window footprint; the geometry box W sits bw px inside it
 * (titlebar_get_area), so every frame node is positioned relative to (bw,bw).
 *
 * Only active in a SceneFX build (-Dscenefx=enabled): without it the frame is
 * disabled and the old border/corner renderers keep working unchanged.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <wayland-server-core.h>

#include "scenefx_compat.h"
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#include "somewm_types.h"
#include "globalconf.h"
#include "objects/client.h"
#include "window.h"
#include "rounded.h"
#include "macos_frame.h"
#include "objects/signal.h"
#include "common/util.h"

/* Defined in somewm.c (non-static); forward-declared here so the appearance
 * switch can re-apply every client's frame without pulling in the lifecycle
 * module. */
extern void apply_geometry_to_wlroots(Client *c);

/* ========== Global appearance mode ========== */

bool
appearance_is_dark(void)
{
	return globalconf.appearance.appearance_dark;
}

static bool
appearance_set(int dark)
{
	if (dark == globalconf.appearance.appearance_dark)
		return false;
	globalconf.appearance.appearance_dark = dark ? true : false;
	return true;
}

/* ========== Per-window frame ========== */

#ifdef HAVE_SCENEFX
/* The C-side mirror of client_is_unmanaged() (override-redirect XWayland). */
static bool
macos_frame_unmanaged(client_t *c)
{
#ifdef XWAYLAND
	return c->client_type == X11 && c->surface.xwayland
		&& c->surface.xwayland->override_redirect;
#else
	(void)c;
	return false;
#endif
}
#endif

/* The frame only decorates managed, decorated, non-fullscreen,
 * non-maximized clients. titlebars_enabled == false shows up here as "no
 * titlebar scene buffer", which is how the C side sees the Lua-side flag. */
bool
client_macos_frame_active(client_t *c)
{
#ifndef HAVE_SCENEFX
	return false;
#else
	int i;

	if (!c || !c->scene || !c->scene_surface)
		return false;
	if (macos_frame_unmanaged(c))
		return false;
	if (c->fullscreen)
		return false;
	/* Maximized windows own the whole workarea: no frame (and none comes
	 * back until the window is restored to a normal size). */
	if (c->maximized || c->maximized_horizontal || c->maximized_vertical)
		return false;
	for (i = 0; i < CLIENT_TITLEBAR_COUNT; i++) {
		/* A size-zero bar is hidden (titlebars_enabled == false, or the
		 * engine collapsed it): it must not keep the frame alive. */
		if (c->titlebar[i].size > 0 && c->titlebar[i].scene_buffer)
			return true;
	}
	return false;
#endif
}

/* Resolved appearance for a window: per-window override, else global. */
int
client_macos_frame_appearance(client_t *c)
{
	if (c->frame_appearance == 1)
		return 1;
	if (c->frame_appearance == 2)
		return 0;
	return globalconf.appearance.appearance_dark ? 1 : 0;
}

/* Effective corner radii the frame draws with: an explicit per-client
 * corner_radius override wins (the frame follows the client's own shape,
 * so rules and focus-space round/square live), otherwise the Big Sur
 * approximation radius (10 px). */
void
client_macos_frame_radii(client_t *c, int radii[4])
{
	const rounded_config_t *cfg = c->rounded_config;
	int i;

	if (cfg && cfg->enabled
			&& (cfg->radii[ROUNDED_TL] > 0 || cfg->radii[ROUNDED_TR] > 0
				|| cfg->radii[ROUNDED_BL] > 0
				|| cfg->radii[ROUNDED_BR] > 0)) {
		for (i = 0; i < 4; i++)
			radii[i] = cfg->radii[i] < 0 ? 0 : cfg->radii[i];
		return;
	}
	for (i = 0; i < 4; i++)
		radii[i] = MACOS_FRAME_RADIUS;
}

/* The ring buffers never intercept clicks (a NULL point_accepts_input would
 * make wlr_scene_node_at return them as the hit, swallowing input). */
static bool
macos_frame_point_accepts_input(struct wlr_scene_buffer *buffer,
				double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* Apply the monitor-clamp visibility and the window opacity fade to the
 * frame's scene nodes. */
static void
macos_frame_apply_visibility(client_t *c)
{
	struct macos_frame_nodes *f = &c->macos_frame;
	bool on = client_macos_frame_active(c) && f->shown;

	if (f->stroke)
		wlr_scene_node_set_enabled(&f->stroke->node, on);
	if (f->highlight)
		wlr_scene_node_set_enabled(&f->highlight->node, on);
	if (f->shadow_tree)
		wlr_scene_node_set_enabled(&f->shadow_tree->node, on);
#ifdef HAVE_SCENEFX
	if (f->shadow)
		wlr_scene_node_set_enabled(&f->shadow->node, on);
#endif
}

#ifdef HAVE_SCENEFX
static const float shadow_focused_color[4] = MACOS_FRAME_SHADOW_FOCUSED_COLOR;
static const float shadow_unfocused_color[4] = MACOS_FRAME_SHADOW_UNFOCUSED_COLOR;
#endif

#ifdef HAVE_SCENEFX
/* Apply the single SceneFX shadow node for the current focus/appearance
 * state.
 *
 * The node box is the shadow's render canvas: the SceneFX shader derives the
 * opaque silhouette as the node box inset by blur_sigma on every side
 * (box_shadow.frag draws the box from position+blur_sigma to
 * position+size-blur_sigma). To make that silhouette land exactly on W (grown
 * by `spread` and shifted down by `offset_y`), the node is sized
 * W + 2*(spread+blur) and positioned at (bw - spread - blur,
 * bw + offset_y - spread - blur) inside the scene.
 *
 * The clipped region is the window rect W in node-relative coordinates, cut
 * out so the shadow never shows through a translucent window. */
static void
macos_frame_shadow_apply(client_t *c, bool focused, int dark,
			 int spread, float blur, int offset_y)
{
	struct macos_frame_nodes *f = &c->macos_frame;
	struct wlr_scene_shadow *sfx = f->shadow;
	float color[4];
	int bw, w, h;
	int corner, i;
	int node_w, node_h, node_x, node_y;

	if (!sfx)
		return;

	memcpy(color, focused ? shadow_focused_color : shadow_unfocused_color,
		sizeof(color));
	color[3] *= f->shadow_fade;

	bw = c->bw;
	w = c->geometry.width;
	h = c->geometry.height;

	/* SceneFX shadows take a single corner radius: use the widest corner the
	 * frame draws with (the frame radii, or the Big Sur 10 px default). */
	{
		int frame_radii[4];
		client_macos_frame_radii(c, frame_radii);
		corner = frame_radii[ROUNDED_TL];
		for (i = 1; i < 4; i++) {
			if (frame_radii[i] > corner)
				corner = frame_radii[i];
		}
	}

	node_w = w + 2 * (spread + (int)blur);
	node_h = h + 2 * (spread + (int)blur);
	node_x = bw - spread - (int)blur;
	node_y = bw + offset_y - spread - (int)blur;

	wlr_scene_shadow_set_size(sfx, node_w, node_h);
	wlr_scene_shadow_set_corner_radius(sfx, corner);
	wlr_scene_shadow_set_blur_sigma(sfx, blur);
	wlr_scene_shadow_set_color(sfx, color);
	/* Clip the shadow out under the window: node-relative rect of W. */
	wlr_scene_shadow_set_clipped_region(sfx, (struct clipped_region){
		.area = (struct wlr_box){
			.x = spread + (int)blur,
			.y = spread + (int)blur - offset_y,
			.width = w,
			.height = h,
		},
		.corners = corner_radii_all(corner),
	});
	wlr_scene_node_set_position(&sfx->node, node_x, node_y);

	f->shadow_focused = focused;
	f->shadow_cache_dark = dark;
	f->shadow_spread = spread;
	f->shadow_blur = blur;
}
#endif /* HAVE_SCENEFX */

void
client_macos_frame_set_shown(client_t *c, bool shown)
{
	if (!c)
		return;
	c->macos_frame.shown = shown;
	macos_frame_apply_visibility(c);
}

void
client_macos_frame_set_fade(client_t *c, float opacity)
{
	struct macos_frame_nodes *f;
	bool focused, dark;

	if (!c || !c->scene)
		return;
	f = &c->macos_frame;
	if (opacity < 0.0f)
		opacity = 0.0f;
	if (opacity > 1.0f)
		opacity = 1.0f;
	f->shadow_fade = opacity;
	if (f->stroke)
		wlr_scene_buffer_set_opacity(f->stroke, opacity);
	if (f->highlight)
		wlr_scene_buffer_set_opacity(f->highlight, opacity);
	if (f->shadow && f->shadow_cache_dark >= 0) {
		focused = f->shadow_focused;
		dark = f->shadow_cache_dark;
#ifdef HAVE_SCENEFX
		int spread = f->shadow_spread;
		float blur = f->shadow_blur;
		int offset_y = focused ? MACOS_FRAME_SHADOW_FOCUSED_OFFSET_Y
			: MACOS_FRAME_SHADOW_UNFOCUSED_OFFSET_Y;
		macos_frame_shadow_apply(c, focused, dark, spread, blur, offset_y);
#else
		(void)focused;
		(void)dark;
#endif
	}
}

void
client_macos_frame_update(client_t *c)
{
	struct macos_frame_nodes *f;
	int dark, bw, frame_w, frame_h;
	int radii[4];
	int stroke_radii[4];
	const float stroke_dark[4] = MACOS_FRAME_STROKE_COLOR_DARK;
	const float stroke_light[4] = MACOS_FRAME_STROKE_COLOR_LIGHT;
	const float side[4] = MACOS_FRAME_HIGHLIGHT_SIDE;
	const float top[4] = MACOS_FRAME_HIGHLIGHT_TOP;

	if (!c || !c->scene || !c->scene_surface)
		return;

	f = &c->macos_frame;
	if (!client_macos_frame_active(c)) {
		macos_frame_apply_visibility(c);
		return;
	}

	dark = client_macos_frame_appearance(c);
	bw = c->bw;
	frame_w = c->geometry.width;
	frame_h = c->geometry.height;
	client_macos_frame_radii(c, radii);
	for (int i = 0; i < 4; i++)
		stroke_radii[i] = radii[i] + MACOS_FRAME_STROKE_WIDTH;

	/* Outer stroke: 1 px ring just OUTSIDE the window edge, rendered into a
	 * buffer two pixels larger, positioned at (bw-1,bw-1) so the ring hugs
	 * W's rounded rect (outer radius R+1, inner radius R = W's edge). */
	if (!f->stroke) {
		f->stroke = wlr_scene_buffer_create(c->scene, NULL);
		if (!f->stroke)
			return;
		f->stroke->point_accepts_input = macos_frame_point_accepts_input;
		f->stroke->node.data = c->scene->node.data;
		wlr_scene_node_place_below(&f->stroke->node, &c->scene_surface->node);
		f->stroke_cache_dark = -1;
	}
	if (!f->stroke_buf
			|| f->stroke_w != frame_w + 2 || f->stroke_h != frame_h + 2
			|| memcmp(f->stroke_radii, stroke_radii,
				sizeof(f->stroke_radii)) != 0
			|| f->stroke_cache_dark != dark) {
		struct wlr_buffer *buf = rounded_crop_render_ring(
			frame_w + 2, frame_h + 2, MACOS_FRAME_STROKE_WIDTH,
			stroke_radii, dark ? stroke_dark : stroke_light);
		if (!buf)
			return;
		if (f->stroke_buf)
			wlr_buffer_drop(f->stroke_buf);
		f->stroke_buf = buf;
		f->stroke_w = frame_w + 2;
		f->stroke_h = frame_h + 2;
		memcpy(f->stroke_radii, stroke_radii, sizeof(f->stroke_radii));
		memcpy(f->stroke_color, dark ? stroke_dark : stroke_light,
			sizeof(f->stroke_color));
		f->stroke_cache_dark = dark;
		wlr_scene_buffer_set_buffer(f->stroke, buf);
		wlr_scene_buffer_set_dest_size(f->stroke, frame_w + 2, frame_h + 2);
		if (f->shadow_fade < 1.0f)
			wlr_scene_buffer_set_opacity(f->stroke, f->shadow_fade);
	}
	wlr_scene_node_set_position(&f->stroke->node, bw - 1, bw - 1);

	/* Inner highlight: dark only, 1 px ring INSIDE W, above content and
	 * titlebars. In light appearance there is no node at all, so switching
	 * dark -> light destroys it (and its buffer). */
	if (!dark) {
		if (f->highlight) {
			if (f->highlight_buf) {
				wlr_buffer_drop(f->highlight_buf);
				f->highlight_buf = NULL;
			}
			wlr_scene_node_destroy(&f->highlight->node);
			f->highlight = NULL;
			f->hl_w = f->hl_h = 0;
		}
	} else {
		if (!f->highlight) {
			f->highlight = wlr_scene_buffer_create(c->scene, NULL);
			if (!f->highlight)
				return;
			f->highlight->point_accepts_input =
				macos_frame_point_accepts_input;
			f->highlight->node.data = c->scene->node.data;
		}
		if (!f->highlight_buf
				|| f->hl_w != frame_w || f->hl_h != frame_h
				|| memcmp(f->hl_radii, radii, sizeof(f->hl_radii)) != 0) {
			struct wlr_buffer *buf = rounded_crop_render_macos_highlight(
				frame_w, frame_h, radii, MACOS_FRAME_HIGHLIGHT_WIDTH,
				side, top);
			if (!buf)
				return;
			if (f->highlight_buf)
				wlr_buffer_drop(f->highlight_buf);
			f->highlight_buf = buf;
			f->hl_w = frame_w;
			f->hl_h = frame_h;
			memcpy(f->hl_radii, radii, sizeof(f->hl_radii));
			wlr_scene_buffer_set_buffer(f->highlight, buf);
			wlr_scene_buffer_set_dest_size(f->highlight, frame_w, frame_h);
			if (f->shadow_fade < 1.0f)
				wlr_scene_buffer_set_opacity(f->highlight,
					f->shadow_fade);
		}
		wlr_scene_node_set_position(&f->highlight->node, bw, bw);
		/* Above content and titlebars; popups must stay on top. */
		wlr_scene_node_raise_to_top(&f->highlight->node);
		if (c->popups)
			wlr_scene_node_raise_to_top(&c->popups->node);
	}

	#ifdef HAVE_SCENEFX
	/* Shadow: one SceneFX layer, re-parameterized on focus/appearance. */
	if (!f->shadow_tree) {
		f->shadow_tree = wlr_scene_tree_create(c->scene);
		if (!f->shadow_tree)
			return;
		wlr_scene_node_lower_to_bottom(&f->shadow_tree->node);
		f->shadow = wlr_scene_shadow_create(f->shadow_tree,
			1, 1, 0, 0, (float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		f->shadow_cache_dark = -1;
		f->shadow_fade = 1.0f;
		f->shown = true;
	}

	bool focused = globalconf.focus.client == c;
	if (f->shadow
			&& (f->shadow_focused != focused
				|| f->shadow_cache_dark != dark
				|| f->shadow_w != frame_w || f->shadow_h != frame_h
				|| f->shadow_corner != radii[ROUNDED_TL])) {
		int offset_y = focused ? MACOS_FRAME_SHADOW_FOCUSED_OFFSET_Y
			: MACOS_FRAME_SHADOW_UNFOCUSED_OFFSET_Y;
		int spread = focused ? MACOS_FRAME_SHADOW_FOCUSED_SPREAD
			: MACOS_FRAME_SHADOW_UNFOCUSED_SPREAD;
		float blur = focused ? MACOS_FRAME_SHADOW_FOCUSED_BLUR
			: MACOS_FRAME_SHADOW_UNFOCUSED_BLUR;
		f->shadow_w = frame_w;
		f->shadow_h = frame_h;
		f->shadow_corner = radii[ROUNDED_TL];
		macos_frame_shadow_apply(c, focused, dark, spread, blur, offset_y);
	}
#endif

	macos_frame_apply_visibility(c);
}

/* Free the frame's owned buffers and forget every scene node (the tree is
 * being destroyed by the caller, so the node pointers are not dereferenced).
 * Mirrors blur_release(): called when the client's scene tree dies. */
void
client_macos_frame_release(client_t *c)
{
	if (!c)
		return;
	if (c->macos_frame.stroke_buf) {
		wlr_buffer_drop(c->macos_frame.stroke_buf);
		c->macos_frame.stroke_buf = NULL;
	}
	if (c->macos_frame.highlight_buf) {
		wlr_buffer_drop(c->macos_frame.highlight_buf);
		c->macos_frame.highlight_buf = NULL;
	}
	memset(&c->macos_frame, 0, sizeof(c->macos_frame));
}

/* Rebuild one client's frame immediately (c.frame_appearance writes). */
void
client_macos_frame_repaint(client_t *c)
{
	if (!c || !c->scene || !c->scene_surface)
		return;
	apply_geometry_to_wlroots(c);
}

void
macos_frame_reload_all(void)
{
	foreach(ci, globalconf.clients) {
		Client *c = *ci;
		if (c->scene && c->scene_surface)
			apply_geometry_to_wlroots(c);
	}
}

/* ========== Global `somewm` Lua object ========== */

static int
somewm_index(lua_State *L)
{
	const char *key = luaL_checkstring(L, 2);

	if (A_STREQ(key, "appearance")) {
		if (appearance_is_dark())
			lua_pushliteral(L, "dark");
		else
			lua_pushliteral(L, "light");
		return 1;
	}
	return 0;
}

static int
somewm_newindex(lua_State *L)
{
	const char *key = luaL_checkstring(L, 2);

	if (A_STREQ(key, "appearance")) {
		const char *val = luaL_checkstring(L, 3);
		int dark;

		if (A_STREQ(val, "dark"))
			dark = 1;
		else if (A_STREQ(val, "light"))
			dark = 0;
		else {
			warn("somewm.appearance: invalid value '%s' "
				"(expected \"dark\" or \"light\"); keeping current",
				val);
			return 0;
		}
		if (appearance_set(dark)) {
			luaA_emit_signal_global("property::appearance");
			macos_frame_reload_all();
		}
		return 0;
	}

	lua_rawset(L, 1);
	return 0;
}

void
somewm_setup(lua_State *L)
{
	lua_newtable(L);                       /* somewm */
	lua_newtable(L);                       /* metatable */
	lua_pushcfunction(L, somewm_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, somewm_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_setmetatable(L, -2);
	lua_setglobal(L, "somewm");
}