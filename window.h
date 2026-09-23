/*
 * window.h - Rounded crop + inner hairline helpers (ported to release/1.4)
 *
 * On upstream main these helpers live in a full window-lifecycle module; on
 * release/1.4 the lifecycle stays in somewm.c and this file only carries the
 * feature-specific surface: rounded-corner crop, the border ring, and the
 * macOS-style inner hairline.
 */
#ifndef WINDOW_H
#define WINDOW_H

#include "somewm_types.h"

struct wl_display;
struct wl_listener;
struct wlr_box;
struct wlr_buffer;

/* Rounded corner crop (true transparency) */
bool client_crop_active(Client *c);
/* Effective per-corner radii (TL,TR,BL,BR) of the outer frame contour; false
 * when rounding is off (disabled, fullscreen, or all radii zero). */
bool client_crop_outer_radii(Client *c, int radii[4]);
bool client_crop_inner_rect(Client *c, struct wlr_box *rect, int radii[4]);
void client_crop_apply(Client *c);
void client_crop_titlebar_buffer(Client *c, struct wlr_buffer *buffer, struct wlr_box area);
void client_crop_config_changed(Client *c);
void client_crop_release(Client *c);

/* Focus-mode reveal: wrap/unwrap the toplevel surface buffers' input callback
 * so the strip that slid past the monitor does not steal input (see
 * somewm.c). Popups are never wrapped. */
void client_offset_input_apply(Client *c);
void client_offset_input_clear(Client *c);
/* Tag slide: block (or restore) pointer input on a client while its desktop
 * is sliding out. Uses the same scene-buffer wrap machinery. */
void client_slide_input_apply(Client *c, bool blocked);
void cropcommitnotify(struct wl_listener *listener, void *data);
void contentcommitnotify(struct wl_listener *listener, void *data);
void client_crop_update_ring(Client *c, int frame_w, int frame_h);
void client_crop_update_innerline(Client *c);
/* Whether the inner hairline is enabled and has a live buffer (see window.c). */
bool client_crop_innerline_active(Client *c);

/* Inner hairline (macOS-style line inside the border) */
unsigned int get_border_inner_width(void);
const float *get_border_inner_color(void);
bool get_border_inner_enabled(void);
bool get_border_inner_drawin_enabled(void);
void client_init_border_inner_defaults(Client *c);

/* Layer-surface corner crop (opt-in via Lua `corner_radius`) */
void layer_surface_crop_apply(LayerSurface *l);
void layer_surface_crop_release(LayerSurface *l);
void layer_surface_crop_reload(void);
void layer_surface_cropcommitnotify(struct wl_listener *listener, void *data);

#ifdef HAVE_SCENEFX
/* SceneFX shader rounding: replaces the CPU crop path when compiled in. */
void client_scenefx_apply_radii(Client *c);
void client_scenefx_update_border(Client *c, int frame_w, int frame_h);
void layer_surface_scenefx_apply_radii(LayerSurface *l);
#endif

/* SceneFX backdrop blur (scene access is stubbed in a -Dscenefx=disabled
 * build, so these are safe to call unconditionally). */
void client_blur_update(Client *c);
void layer_surface_blur_update(LayerSurface *l);

/* Opacity-correct fades: one buffer walk plus per-decoration alpha.
 * Vanilla scene API only, so safe to call in both builds. */
struct wlr_scene_node;
void scene_apply_opacity(struct wlr_scene_node *node, float opacity);
void client_fade_apply(Client *c, float opacity);
/* Record an unfaded border color and re-apply it scaled by current opacity. */
void client_border_set_base(Client *c, const float color[static 4]);

#endif /* WINDOW_H */