/*
 * blur.c - backdrop blur for clients and layer surfaces (SceneFX 0.5)
 *
 * Without SceneFX this file is inert: every function is a no-op or returns
 * the compiled-in default, and no wlr_scene_blur node is ever created, so a
 * `-Dscenefx=disabled` build behaves exactly as before. The Lua-side
 * backdrop_blur properties still store their config there, it just has no
 * scene effect until SceneFX is enabled.
 */

#include "blur.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SCENEFX
#include <wlr/util/log.h>

/* Global blur device parameters, applied once at startup and overridable
 * from Lua via awesome.set_blur_data() / set_blur_preset(). macOS-like
 * defaults reconstructed from Apple's backdrop filters: a ~30pt gaussian
 * computed at 1/4 scale (passes 2, radius 5), strong saturation, no grain. */
static int blur_data_num_passes = 2;
static int blur_data_radius = 5;
static float blur_data_noise = 0.0f;
static float blur_data_brightness = 1.0f;
static float blur_data_contrast = 1.0f;
static float blur_data_saturation = 1.8f;

/* Protocol panel blur mode (live by default; optimized is opt-in). */
static blur_mode_t blur_mode = BLUR_MODE_LIVE;

#endif /* HAVE_SCENEFX */

bool
blur_config_from_lua(lua_State *L, int idx, blur_config_t *config)
{
	int type = lua_type(L, idx);

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;

	if (type == LUA_TNIL || type == LUA_TNONE) {
		config->enabled = false;
		return true;
	}
	if (type == LUA_TBOOLEAN) {
		config->enabled = lua_toboolean(L, idx);
		config->corner_radius = 0;
		config->alpha = 1.0f;
		config->strength = 1.0f;
		return true;
	}
	if (type == LUA_TTABLE) {
		config->enabled = true;
		config->corner_radius = 0;
		config->alpha = 1.0f;
		config->strength = 1.0f;
		lua_getfield(L, idx, "corner_radius");
		if (lua_isnumber(L, -1))
			config->corner_radius = lua_tointeger(L, -1);
		lua_getfield(L, idx, "alpha");
		if (lua_isnumber(L, -1))
			config->alpha = lua_tonumber(L, -1);
		lua_getfield(L, idx, "strength");
		if (lua_isnumber(L, -1))
			config->strength = lua_tonumber(L, -1);
		lua_pop(L, 3);
		return true;
	}
	lua_pushliteral(L, "invalid value for blur config (expected boolean or table)");
	return false;
}

void
blur_config_to_lua(lua_State *L, const blur_config_t *config)
{
	if (!config || !config->enabled) {
		lua_pushboolean(L, false);
		return;
	}
	if (config->corner_radius == 0 && config->alpha == 1.0f
			&& config->strength == 1.0f) {
		lua_pushboolean(L, true);
		return;
	}
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, config->corner_radius);
	lua_setfield(L, -2, "corner_radius");
	lua_pushnumber(L, config->alpha);
	lua_setfield(L, -2, "alpha");
	lua_pushnumber(L, config->strength);
	lua_setfield(L, -2, "strength");
}

void
blur_set_fade(blur_nodes_t *blur, float fade)
{
#ifdef HAVE_SCENEFX
	if (!blur || !blur->node) {
		blur->fade = fade;
		return;
	}
	blur->fade = fade;
	wlr_scene_blur_set_alpha(blur->node, blur->config.alpha * fade);
	wlr_scene_blur_set_strength(blur->node, blur->config.strength * fade);
#else
	(void)blur; (void)fade;
#endif
}

void
blur_set_only_bottom_layer(blur_nodes_t *blur, bool only_bottom)
{
#ifdef HAVE_SCENEFX
	if (!blur || !blur->node)
		return;
	wlr_scene_blur_set_should_only_blur_bottom_layer(blur->node, only_bottom);
#else
	(void)blur; (void)only_bottom;
#endif
}

blur_mode_t
blur_get_mode(void)
{
#ifdef HAVE_SCENEFX
	return blur_mode;
#else
	return BLUR_MODE_LIVE;
#endif
}

void
blur_set_mode(blur_mode_t mode)
{
#ifdef HAVE_SCENEFX
	if (mode == BLUR_MODE_LIVE || mode == BLUR_MODE_OPTIMIZED)
		blur_mode = mode;
#endif
}

#ifdef HAVE_SCENEFX

/* Apply the cached burst parameters to a (re)created scene. */
static void
blur_apply_device_data(struct wlr_scene *scene)
{
	if (!scene)
		return;
	wlr_log(WLR_INFO, "blur device data: passes=%d radius=%d noise=%.2f "
		"brightness=%.2f contrast=%.2f saturation=%.2f",
		blur_data_num_passes, blur_data_radius, blur_data_noise,
		blur_data_brightness, blur_data_contrast, blur_data_saturation);
	wlr_scene_set_blur_data(scene, blur_data_num_passes, blur_data_radius,
		blur_data_noise, blur_data_brightness, blur_data_contrast,
		blur_data_saturation);
}

void
blur_set_data(struct wlr_scene *scene, int num_passes, int radius,
	float noise, float brightness, float contrast, float saturation)
{
	blur_data_num_passes = num_passes;
	blur_data_radius = radius;
	blur_data_noise = noise;
	blur_data_brightness = brightness;
	blur_data_contrast = contrast;
	blur_data_saturation = saturation;
	blur_apply_device_data(scene);
}

/* Apply a named blur preset; returns false for an unknown name. */
bool
blur_set_preset(struct wlr_scene *scene, const char *name)
{
	if (strcmp(name, "macos") == 0) {
		blur_set_data(scene, 2, 5, 0.0f, 1.0f, 1.0f, 1.8f);
		return true;
	}
	if (strcmp(name, "strong") == 0) {
		blur_set_data(scene, 3, 5, 0.0f, 1.0f, 1.0f, 1.8f);
		return true;
	}
	return false;
}

void
blur_setup_defaults(struct wlr_scene *scene)
{
	blur_apply_device_data(scene);
}

void
blur_apply(struct wlr_scene_tree *parent, blur_nodes_t *blur,
	const blur_config_t *config, const struct wlr_box *area,
	const int radii[4], struct wlr_scene_buffer *mask)
{
	struct wlr_scene_blur *blur_node;
	struct fx_corner_radii corners;
	float fade;

	if (!config || !config->enabled || !area || area->width <= 0
			|| area->height <= 0) {
		blur_release(blur);
		return;
	}

	blur->config = *config;

	blur_node = blur->node;
	if (!blur_node) {
		blur_node = wlr_scene_blur_create(parent, area->width,
			area->height);
		if (!blur_node) {
			return;
		}
		blur->node = blur_node;
		blur->fade = 1.0f;
	}

	fade = blur->fade;

	/* Radii: an explicit corner_radius from the config wins, else the
	 * object's own radii. The radii array is in rounded-config order
	 * (TL, TR, BL, BR), fx_corner_radii is (TL, TR, BR, BL), so the
	 * bottom two swap. */
	if (config->corner_radius > 0) {
		corners = corner_radii_new(config->corner_radius,
			config->corner_radius, config->corner_radius,
			config->corner_radius);
	} else if (radii) {
		corners = corner_radii_new(radii[0], radii[1], radii[3], radii[2]);
	} else {
		corners = corner_radii_none();
	}

	wlr_scene_blur_set_size(blur_node, area->width, area->height);
	wlr_scene_blur_set_corner_radii(blur_node, corners);
	wlr_scene_blur_set_alpha(blur_node, blur->config.alpha * fade);
	wlr_scene_blur_set_strength(blur_node, blur->config.strength * fade);
	/* Always re-link: passing NULL releases a formerly linked mask, and a
	 * stale link may point at a buffer the rest of the tree freed. */
	wlr_scene_blur_set_transparency_mask_source(blur_node, mask);
	wlr_scene_node_set_position(&blur_node->node, area->x, area->y);
	wlr_scene_node_set_enabled(&blur_node->node, true);
}

void
blur_release(blur_nodes_t *blur)
{
	if (!blur)
		return;
	if (blur->node) {
		wlr_scene_node_destroy(&blur->node->node);
		blur->node = NULL;
	}
	blur->fade = 1.0f;
}

#else /* !HAVE_SCENEFX */

void
blur_set_data(struct wlr_scene *scene, int num_passes, int radius,
	float noise, float brightness, float contrast, float saturation)
{
	(void)scene; (void)num_passes; (void)radius; (void)noise;
	(void)brightness; (void)contrast; (void)saturation;
}

bool
blur_set_preset(struct wlr_scene *scene, const char *name)
{
	(void)scene; (void)name;
	return false;
}

void
blur_setup_defaults(struct wlr_scene *scene)
{
	(void)scene;
}

void
blur_apply(struct wlr_scene_tree *parent, blur_nodes_t *blur,
	const blur_config_t *config, const struct wlr_box *area,
	const int radii[4], struct wlr_scene_buffer *mask)
{
	(void)parent; (void)blur; (void)config; (void)area;
	(void)radii; (void)mask;
}

void
blur_release(blur_nodes_t *blur)
{
	(void)blur;
}

#endif /* HAVE_SCENEFX */