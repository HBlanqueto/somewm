/*
 * background_effect.c - ext-background-effect-v1 (backdrop blur)
 *
 * Server side of the ext-background-effect-v1 protocol. A client asks for a
 * backdrop blur on its surface with set_blur_region; the region is
 * double-buffered and applied on the surface's commit, clipped to the surface
 * and only honored for layer-shell surfaces in the namespace allowlist. The
 * blur is rendered as a SceneFX blur node plus an optimized-blur node that
 * caches the blurred wallpaper behind the panel.
 *
 * Without SceneFX this module still answers the protocol (so clients behave),
 * but no blur node is ever created.
 */

#include "background_effect.h"

#include <stdlib.h>
#include <string.h>

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>

#include "ext-background-effect-v1-protocol.h"
#include "scenefx_compat.h"
#include "somewm_types.h"
#include "window.h"

/* Layer-shell namespaces allowed to receive a protocol backdrop blur.
 * Extend with menus, the dock and the notch as they gain translucency. */
static const char *const bg_blur_namespaces[] = {
	"quickshell:panel",
};

struct bg_effect_surface {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct LayerSurface *ls; /* layer surface the blur is applied to (NULL = off) */

	pixman_region32_t pending; /* set_blur_region state, surface-local */
	pixman_region32_t applied; /* region copied on commit */

	bool inert; /* wl_surface was destroyed */

	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;

	struct wl_list link; /* bg_effects */
};

static struct wl_list bg_effects;

static bool
bg_blur_namespace_match(const char *ns)
{
	size_t i;
	for (i = 0; i < sizeof(bg_blur_namespaces) / sizeof(bg_blur_namespaces[0]); i++)
		if (strcmp(ns, bg_blur_namespaces[i]) == 0)
			return true;
	return false;
}

/* Disable the blur on the layer surface this effect was applied to. */
static void
bg_effect_clear(struct bg_effect_surface *effect)
{
	struct LayerSurface *l = effect->ls;

	if (!l)
		return;
	l->bg_blur_enabled = false;
	effect->ls = NULL;
	layer_surface_blur_update(l);
}

static void
bg_effect_surface_commit(struct wl_listener *listener, void *data)
{
	struct bg_effect_surface *effect =
		wl_container_of(listener, effect, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface;
	struct LayerSurface *l;

	(void)data;

	pixman_region32_copy(&effect->applied, &effect->pending);

	layer_surface = wlr_layer_surface_v1_try_from_wlr_surface(effect->surface);
	l = layer_surface ? layer_surface->data : NULL;

	if (!l || !l->layer_surface->namespace
			|| !bg_blur_namespace_match(l->layer_surface->namespace)
			|| pixman_region32_empty(&effect->applied)) {
		bg_effect_clear(effect);
		return;
	}

	if (effect->ls != l) {
		bg_effect_clear(effect);
		effect->ls = l;
	}

	l->bg_blur_enabled = true;
	pixman_region32_copy(&l->bg_blur_region, &effect->applied);
	layer_surface_blur_update(l);
}

/* The wl_surface is gone; the layer surface (if any) was already destroyed and
 * called background_effect_layer_surface_destroy(), so ls is NULL here. */
static void
bg_effect_surface_destroy(struct wl_listener *listener, void *data)
{
	struct bg_effect_surface *effect =
		wl_container_of(listener, effect, surface_destroy);

	(void)data;
	effect->inert = true;
	wl_list_remove(&effect->surface_commit.link);
	wl_list_remove(&effect->surface_destroy.link);
}

static void
bg_effect_resource_destroy(struct wl_resource *resource)
{
	struct bg_effect_surface *effect = wl_resource_get_user_data(resource);

	bg_effect_clear(effect);
	if (!effect->inert) {
		wl_list_remove(&effect->surface_commit.link);
		wl_list_remove(&effect->surface_destroy.link);
	}
	wl_list_remove(&effect->link);
	pixman_region32_fini(&effect->pending);
	pixman_region32_fini(&effect->applied);
	free(effect);
}

static void
bg_effect_set_blur_region(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *region_resource)
{
	struct bg_effect_surface *effect = wl_resource_get_user_data(resource);
	const pixman_region32_t *region;

	(void)client;

	if (region_resource) {
		region = wlr_region_from_resource(region_resource);
		pixman_region32_copy(&effect->pending, region);
	} else {
		pixman_region32_clear(&effect->pending);
	}
}

static void
bg_effect_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_background_effect_surface_v1_interface
bg_effect_surface_impl = {
	.destroy = bg_effect_destroy,
	.set_blur_region = bg_effect_set_blur_region,
};

static void
bg_manager_get_background_effect(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wl_resource_get_user_data(surface_resource);
	struct bg_effect_surface *effect;
	struct wl_resource *effect_resource;

	if (strcmp(wl_resource_get_class(surface_resource), wl_surface_interface.name) != 0) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			"invalid surface");
		return;
	}

	wl_list_for_each(effect, &bg_effects, link) {
		if (effect->surface == surface) {
			wl_resource_post_error(resource,
				EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
				"surface already has a background effect object");
			return;
		}
	}

	effect = calloc(1, sizeof(*effect));
	if (!effect) {
		wl_client_post_no_memory(client);
		return;
	}
	effect->surface = surface;
	pixman_region32_init(&effect->pending);
	pixman_region32_init(&effect->applied);

	effect_resource = wl_resource_create(client,
		&ext_background_effect_surface_v1_interface, 1, id);
	if (!effect_resource) {
		pixman_region32_fini(&effect->pending);
		pixman_region32_fini(&effect->applied);
		free(effect);
		wl_client_post_no_memory(client);
		return;
	}
	effect->resource = effect_resource;
	wl_resource_set_implementation(effect_resource, &bg_effect_surface_impl,
		effect, bg_effect_resource_destroy);

	effect->surface_commit.notify = bg_effect_surface_commit;
	wl_signal_add(&surface->events.commit, &effect->surface_commit);
	effect->surface_destroy.notify = bg_effect_surface_destroy;
	wl_signal_add(&surface->events.destroy, &effect->surface_destroy);

	wl_list_insert(&bg_effects, &effect->link);
}

static void
bg_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_background_effect_manager_v1_interface bg_manager_impl = {
	.destroy = bg_manager_destroy,
	.get_background_effect = bg_manager_get_background_effect,
};

static void
bg_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &bg_manager_impl, NULL, NULL);
	ext_background_effect_manager_v1_send_capabilities(resource,
		EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

void
background_effect_init(struct wl_display *dpy)
{
	wl_list_init(&bg_effects);
	wl_global_create(dpy, &ext_background_effect_manager_v1_interface, 1,
		NULL, bg_manager_bind);
}

void
background_effect_invalidate(void)
{
	struct bg_effect_surface *effect;

	wl_list_for_each(effect, &bg_effects, link) {
		struct LayerSurface *l = effect->ls;
#ifdef HAVE_SCENEFX
		if (l && l->bg_blur_optimized)
			wlr_scene_optimized_blur_mark_dirty(l->bg_blur_optimized);
#else
		(void)l;
#endif
	}
}

void
background_effect_layer_surface_destroy(struct LayerSurface *l)
{
	struct bg_effect_surface *effect;

	wl_list_for_each(effect, &bg_effects, link) {
		if (effect->ls == l) {
			/* The scene tree and its nodes are freed by the layer surface
			 * destroy path; just drop the reference. */
			effect->ls = NULL;
		}
	}
}