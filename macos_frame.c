/*
 * macos_frame.c - built-in macOS Big Sur window frame (SceneFX)
 *
 * Draws the whole decorated window frame natively in C: the outer stroke, the
 * inner highlight and the two-layer drop shadow, plus the compositor-wide
 * appearance mode (somewm.appearance) that drives them. The Lua config no
 * longer defines or re-applies a frame; everything is a compile-time constant
 * (macos_frame.h) except the three documented Lua knobs.
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

/* ========== beautiful.macos_frame ========== */

bool
macos_frame_get_enabled(void)
{
	return globalconf.appearance.macos_frame_enabled;
}

void
macos_frame_load_beautiful_defaults(lua_State *L)
{
	if (!L)
		L = globalconf_get_lua_State();
	globalconf.appearance.macos_frame_enabled = true;
	if (!L)
		return;

	lua_getglobal(L, "require");
	lua_pushstring(L, "beautiful");
	if (lua_pcall(L, 1, 1, 0) != 0) {
		lua_pop(L, 1);
		return;
	}
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "macos_frame");
		if (!lua_isnil(L, -1))
			globalconf.appearance.macos_frame_enabled = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
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

/* The frame only decorates managed, decorated, non-fullscreen clients.
 * titlebars_enabled == false shows up here as "no titlebar scene buffer",
 * which is how the C side sees the Lua-side flag. */
bool
client_macos_frame_active(client_t *c)
{
#ifndef HAVE_SCENEFX
	return false;
#else
	int i;

	if (!c || !c->scene || !c->scene_surface)
		return false;
	if (!globalconf.appearance.macos_frame_enabled)
		return false;
	if (macos_frame_unmanaged(c))
		return false;
	if (c->fullscreen)
		return false;
	/* Maximized-to-workarea with the border stripped (focus_space): the
	 * window owns the whole screen, so there is no frame. */
	if ((c->maximized || c->maximized_horizontal || c->maximized_vertical)
			&& c->bw == 0)
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
	int i;

	if (f->stroke)
		wlr_scene_node_set_enabled(&f->stroke->node, on);
	if (f->highlight)
		wlr_scene_node_set_enabled(&f->highlight->node, on);
	if (f->shadow_tree)
		wlr_scene_node_set_enabled(&f->shadow_tree->node, on);
#ifdef HAVE_SCENEFX
	for (i = 0; i < 2; i++) {
		if (f->shadow[i])
			wlr_scene_node_set_enabled(&f->shadow[i]->node, on);
	}
#else
	(void)i;
#endif
}

#ifdef HAVE_SCENEFX
static const float shadow_focused_c1[4] = MACOS_FRAME_SHADOW_FOCUSED_COLOR_1;
static const float shadow_focused_c2[4] = MACOS_FRAME_SHADOW_FOCUSED_COLOR_2;
static const float shadow_unfocused_c1[4] = MACOS_FRAME_SHADOW_UNFOCUSED_COLOR_1;
static const float shadow_unfocused_c2[4] = MACOS_FRAME_SHADOW_UNFOCUSED_COLOR_2;
#endif

#ifdef HAVE_SCENEFX
/* Shadow parameters for the current focus/appearance state, mirroring
 * shadow_sfx_update_geometry()'s node-box derivation (the SceneFX shader
 * derives the shadow rect as the node box inset by blur_sigma). */
static void
macos_frame_shadow_apply(client_t *c, bool focused, int dark)
{
	struct macos_frame_nodes *f = &c->macos_frame;
	struct wlr_scene_shadow *sfx;
	int offset_y, w, h;
	int corner, i;
	float blur1, blur2;
	float color1[4], color2[4];
	int x, y;

	if (focused) {
		offset_y = MACOS_FRAME_SHADOW_FOCUSED_OFFSET_Y;
		blur1 = MACOS_FRAME_SHADOW_FOCUSED_BLUR_1;
		blur2 = MACOS_FRAME_SHADOW_FOCUSED_BLUR_2;
		memcpy(color1, shadow_focused_c1, sizeof(color1));
		memcpy(color2, shadow_focused_c2, sizeof(color2));
	} else {
		offset_y = MACOS_FRAME_SHADOW_UNFOCUSED_OFFSET_Y;
		blur1 = MACOS_FRAME_SHADOW_UNFOCUSED_BLUR_1;
		blur2 = MACOS_FRAME_SHADOW_UNFOCUSED_BLUR_2;
		memcpy(color1, shadow_unfocused_c1, sizeof(color1));
		memcpy(color2, shadow_unfocused_c2, sizeof(color2));
	}
	color1[3] *= f->shadow_fade;
	color2[3] *= f->shadow_fade;

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

	/* Layer 1: the diffuse drop shadow. */
	sfx = f->shadow[0];
	if (sfx) {
		wlr_scene_shadow_set_size(sfx, w + 2 * (int)blur1,
			h + 2 * (int)blur1);
		wlr_scene_shadow_set_corner_radius(sfx, corner);
		wlr_scene_shadow_set_blur_sigma(sfx, blur1);
		wlr_scene_shadow_set_color(sfx, color1);
		x = -blur1;
		y = offset_y - blur1;
		wlr_scene_node_set_position(&sfx->node, x, y);
	}

	/* Layer 2: the tight contact shadow. */
	sfx = f->shadow[1];
	if (sfx) {
		wlr_scene_shadow_set_size(sfx, w + 2 * (int)blur2,
			h + 2 * (int)blur2);
		wlr_scene_shadow_set_corner_radius(sfx, corner);
		wlr_scene_shadow_set_blur_sigma(sfx, blur2);
		wlr_scene_shadow_set_color(sfx, color2);
		x = -blur2;
		y = -blur2;
		wlr_scene_node_set_position(&sfx->node, x, y);
	}

	f->shadow_focused = focused;
	f->shadow_cache_dark = dark;
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
	if (f->shadow[0]) {
		focused = f->shadow_focused;
		dark = f->shadow_cache_dark;
#ifdef HAVE_SCENEFX
		macos_frame_shadow_apply(c, focused, dark);
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
	int dark, frame_w, frame_h;
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
	frame_w = c->geometry.width;
	frame_h = c->geometry.height;
	client_macos_frame_radii(c, radii);
	for (int i = 0; i < 4; i++)
		stroke_radii[i] = radii[i] + MACOS_FRAME_STROKE_WIDTH;

	/* Outer stroke: 1 px ring just OUTSIDE the window edge, rendered into a
	 * buffer two pixels larger and shifted up-left so the stroke hugs the
	 * window's rounded rect (inner radius 10, outer radius 11). */
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
	wlr_scene_node_set_position(&f->stroke->node, -1, -1);

	/* Inner highlight: dark only. In light appearance there is no node at
	 * all, so switching dark -> light destroys it (and its buffer). */
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
		wlr_scene_node_set_position(&f->highlight->node, 0, 0);
		/* Above content and titlebars; popups must stay on top. */
		wlr_scene_node_raise_to_top(&f->highlight->node);
		if (c->popups)
			wlr_scene_node_raise_to_top(&c->popups->node);
	}

	#ifdef HAVE_SCENEFX
	/* Shadows: two SceneFX layers, re-parameterized on focus/appearance. */
	if (!f->shadow_tree) {
		f->shadow_tree = wlr_scene_tree_create(c->scene);
		if (!f->shadow_tree)
			return;
		wlr_scene_node_lower_to_bottom(&f->shadow_tree->node);
		f->shadow[0] = wlr_scene_shadow_create(f->shadow_tree,
			1, 1, 0, 0, (float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		f->shadow[1] = wlr_scene_shadow_create(f->shadow_tree,
			1, 1, 0, 0, (float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		f->shadow_cache_dark = -1;
		f->shadow_fade = 1.0f;
		f->shown = true;
	}

	bool focused = globalconf.focus.client == c;
	if (f->shadow[0] && f->shadow[1]
			&& (f->shadow_focused != focused
				|| f->shadow_cache_dark != dark
				|| f->shadow_w != frame_w || f->shadow_h != frame_h
				|| f->shadow_corner != radii[ROUNDED_TL])) {
		f->shadow_w = frame_w;
		f->shadow_h = frame_h;
		f->shadow_corner = radii[ROUNDED_TL];
		macos_frame_shadow_apply(c, focused, dark);
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
	macos_frame_load_beautiful_defaults(globalconf_get_lua_State());
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