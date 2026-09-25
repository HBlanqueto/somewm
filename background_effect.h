/*
 * background_effect.h - ext-background-effect-v1 (backdrop blur)
 *
 * Server side of the ext-background-effect-v1 protocol. Only layer-shell
 * surfaces whose namespace is in an allowlist receive a real backdrop blur
 * (currently the top panel). Every other surface's blur request is ignored.
 */

#ifndef SOMEWM_BACKGROUND_EFFECT_H
#define SOMEWM_BACKGROUND_EFFECT_H

#include <wayland-server-core.h>

struct LayerSurface;

/** Create and advertise the ext-background-effect-manager-v1 global. */
void background_effect_init(struct wl_display *dpy);

/**
 * Re-render every active optimized blur cache.
 * Call when a "static" backdrop changes (wallpaper) or the output mode/scale
 * changes so the cached blurred background is rebuilt.
 */
void background_effect_invalidate(void);

/**
 * Drop any background-effect state targeting a layer surface that is being
 * destroyed. Must be called before the LayerSurface is freed.
 */
void background_effect_layer_surface_destroy(struct LayerSurface *l);

#endif /* SOMEWM_BACKGROUND_EFFECT_H */