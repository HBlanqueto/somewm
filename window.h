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
void cropcommitnotify(struct wl_listener *listener, void *data);
void contentcommitnotify(struct wl_listener *listener, void *data);
void client_crop_update_ring(Client *c, int frame_w, int frame_h);
void client_crop_update_innerline(Client *c);

/* Inner hairline (macOS-style line inside the border) */
unsigned int get_border_inner_width(void);
const float *get_border_inner_color(void);
bool get_border_inner_enabled(void);
bool get_border_inner_drawin_enabled(void);
void client_init_border_inner_defaults(Client *c);

#endif /* WINDOW_H */