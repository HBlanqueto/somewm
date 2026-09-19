/*
 * rounded.h - compositor-level rounded corner support
 *
 * Copyright © 2026 somewm contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef SOMEWM_ROUNDED_H
#define SOMEWM_ROUNDED_H

#include <lua.h>
#include <stdbool.h>
#include <stdint.h>
#include "scenefx_compat.h"
#include <wlr/types/wlr_buffer.h>

/** Number of owned corner textures (TL, TR, BL, BR). */
#define ROUNDED_TEXTURE_COUNT 4

/** Corner indexes used everywhere (mask nodes, crop, punch, ring). */
enum {
    ROUNDED_TL = 0,
    ROUNDED_TR = 1,
    ROUNDED_BL = 2,
    ROUNDED_BR = 3,
};

/**
 * Rounded corner configuration for a single object (client or drawin).
 *
 * The corners are rendered per-corner: `radii[]` holds the radius of each
 * quarter-circle cut (TL, TR, BL, BR), so each corner can be rounded
 * independently or disabled by giving it radius 0. `radius` mirrors
 * `radii[ROUNDED_TL]` and is kept for the uniform shorthand: setting a
 * single value applies it to all four corners, and the renderers read
 * `radii[]` as the source of truth.
 *
 * The corners are rendered as four mask square overlays painted with
 * `color` in the area cut away by a quarter-circle at each corner of the
 * object's frame. The desktop (or whatever `color` matches) shows through
 * the cut corners. wlroots 0.20 has no rounded clip in its scene API, so
 * this is the closest scene-based equivalent; `color` should match the
 * background behind the window for a seamless cut.
 *
 * When NULL on an object, global defaults from globalconf are used.
 * When non-NULL, these values override the defaults.
 */
typedef struct rounded_config_t {
    bool enabled;           /**< Corner masking enabled for this object */
    int radius;             /**< Uniform radius (mirrors radii[ROUNDED_TL]) */
    int radii[4];           /**< Per-corner radius (TL, TR, BL, BR; 0 = square) */
    float color[4];         /**< Color painted in the cut corners (RGBA) */
} rounded_config_t;

/** Set all four corners to the same radius. */
static inline void
rounded_config_set_uniform(rounded_config_t *cfg, int radius)
{
    cfg->radius = radius;
    cfg->radii[ROUNDED_TL] = radius;
    cfg->radii[ROUNDED_TR] = radius;
    cfg->radii[ROUNDED_BL] = radius;
    cfg->radii[ROUNDED_BR] = radius;
}

/** True when the config rounds at least one corner. */
static inline bool
rounded_config_active(const rounded_config_t *cfg)
{
    return cfg && cfg->enabled &&
        (cfg->radii[0] > 0 || cfg->radii[1] > 0 ||
         cfg->radii[2] > 0 || cfg->radii[3] > 0);
}

/**
 * Rounded corner scene nodes attached to a client or drawin.
 *
 * A small tree holding four corner mask buffers, drawn above the object's
 * content. Each mask square is `radius` x `radius` with quarter-circle
 * transparency; it hides the object's own corner pixels and reveals the
 * background color through the cut.
 */
typedef struct rounded_nodes_t {
    struct wlr_scene_tree *tree;                        /**< Container for the corner masks */
    struct wlr_scene_buffer *corner[ROUNDED_TEXTURE_COUNT]; /**< Corner mask buffers */
    struct wlr_buffer *textures[ROUNDED_TEXTURE_COUNT]; /**< Owned corner textures */
    int last_width;                                     /**< Cached width to skip redundant updates */
    int last_height;                                    /**< Cached height to skip redundant updates */
    bool user_visible;                                  /**< Visibility requested via rounded_set_visible */
    bool size_ok;                                       /**< Object large enough for the corner masks */
} rounded_nodes_t;

/**
 * Global rounded corner defaults (stored in globalconf.rounded).
 */
typedef struct rounded_defaults_t {
    rounded_config_t client;   /**< Default for clients */
    rounded_config_t drawin;   /**< Default for drawins/wiboxes */
} rounded_defaults_t;

/* ========== Core API ========== */

/**
 * Get effective rounded corner configuration for an object.
 *
 * @param override Object-specific config (may be NULL for defaults)
 * @param is_drawin true for drawin, false for client
 * @return Effective configuration (never NULL)
 */
const rounded_config_t *rounded_get_effective_config(
    const rounded_config_t *override, bool is_drawin);

/**
 * Create rounded corner mask nodes for an object.
 *
 * Renders the four corner textures and creates the mask tree as a child of
 * the given parent tree, drawn above (in front of) other content.
 *
 * @param parent Parent scene tree (client->scene or drawin->scene_tree)
 * @param rounded Rounded nodes structure to populate
 * @param config Rounded configuration to use
 * @param width Object width in pixels
 * @param height Object height in pixels
 * @return true on success, false on failure
 */
bool rounded_create(struct wlr_scene_tree *parent,
                    rounded_nodes_t *rounded,
                    const rounded_config_t *config,
                    int width, int height);

/**
 * Update rounded corner geometry after object resize.
 *
 * Fast operation: just repositions the corner mask nodes.
 * No texture re-rendering.
 *
 * @param rounded Rounded nodes structure
 * @param config Rounded configuration
 * @param width New object width
 * @param height New object height
 */
/* Updates corner mask positions for a new object size. Returns true when the
 * masks actually moved/resized (so callers know to re-raise popups), false
 * when geometry was unchanged, the tree is missing, or the object is too
 * small to host the masks. */
bool rounded_update_geometry(rounded_nodes_t *rounded,
                             const rounded_config_t *config,
                             int width, int height);

/**
 * Update rounded corners after configuration change.
 *
 * Destroys and recreates the corner masks with new textures.
 *
 * @param rounded Rounded nodes structure
 * @param parent Parent scene tree (for recreation)
 * @param config New configuration
 * @param width Object width
 * @param height Object height
 */
void rounded_update_config(rounded_nodes_t *rounded,
                           struct wlr_scene_tree *parent,
                           const rounded_config_t *config,
                           int width, int height);

/**
 * Show or hide rounded corner masks.
 *
 * @param rounded Rounded nodes structure
 * @param visible true to show, false to hide
 */
void rounded_set_visible(rounded_nodes_t *rounded, bool visible);

/**
 * Destroy rounded corner nodes and free owned textures.
 *
 * @param rounded Rounded nodes structure to cleanup
 */
void rounded_destroy(rounded_nodes_t *rounded);

/**
 * Free owned textures and zero the structure WITHOUT destroying the scene
 * nodes. For teardown paths where the parent scene tree has been (or is
 * about to be) destroyed, taking the rounded nodes with it.
 *
 * @param rounded Rounded nodes structure to release
 */
void rounded_release(rounded_nodes_t *rounded);

/* ========== True Crop (clients) ========== */

/**
 * Per-client state for true rounded-corner cropping.
 *
 * Unlike the mask overlays above (which paint a color over the cut
 * corners), cropping makes the cut corner pixels genuinely transparent:
 * after wlroots applies a committed client buffer to the scene, the
 * toplevel buffer is replaced with a copy whose corner pixels have been
 * multiplied by the rounded-rect coverage. Titlebar buffers get the same
 * treatment and the square border rects are swapped for a rounded ring.
 * Whatever is behind the window (wallpaper, other windows) shows through.
 */
typedef struct rounded_crop_t {
    /* Content crop cache */
    struct wlr_buffer *buf;         /**< Owned cropped copy of the toplevel buffer */
    const void *src;                /**< Client buffer the copy was made from */
    struct wlr_box local;           /**< Inner rounded rect, node-local coords */
    int radii[4];                   /**< Inner per-corner radii used for the copy */
    double origin_x, origin_y;      /**< Buffer->node-local mapping used for the copy */
    double scale_x, scale_y;        /**< Buffer->node-local mapping used for the copy */
    bool applied;                   /**< A cropped buffer is currently shown */
    bool dirty;                     /**< Force re-crop + full damage on next apply */
    /* Border ring */
    struct wlr_scene_buffer *ring;  /**< Rounded border ring node (lazy) */
    struct wlr_buffer *ring_buf;    /**< Owned ring texture */
    int ring_w, ring_h;             /**< Ring render cache: frame size */
    int ring_bw;                    /**< Ring render cache: border width */
    int ring_radii[4];              /**< Ring render cache: per-corner radii */
    float ring_color[4];            /**< Ring render cache: color */
    /* Inner hairline (macOS-style light line inside the border) */
    struct wlr_scene_buffer *innerline; /**< Inner hairline node (lazy) */
    struct wlr_buffer *innerline_buf;   /**< Owned innerline texture */
    int innerline_w, innerline_h;       /**< Innerline cache: content size */
    int innerline_bw;                   /**< Innerline cache: border width */
    double innerline_line;              /**< Innerline cache: line width */
    int innerline_radii[4];             /**< Innerline cache: radii */
    float innerline_color[4];           /**< Innerline cache: effective color */
} rounded_crop_t;

/**
 * Multiply premultiplied ARGB8888 pixels by the coverage of a rounded
 * rectangle so pixels outside its arcs become transparent.
 *
 * Only the four corner squares of `rrect` are touched; the buffer is
 * assumed to already be clipped to the rectangle itself.
 *
 * Pixel (px, py) maps to local coordinates
 *   (origin_x + (px + 0.5) / scale_x, origin_y + (py + 0.5) / scale_y),
 * which must be the same coordinate space as `rrect`. scale_x/scale_y are
 * buffer pixels per local unit and may differ (viewport scaling).
 *
 * @param pixels Buffer pixels (premultiplied ARGB8888, little endian)
 * @param stride Row stride in bytes
 * @param buf_w Buffer width in pixels
 * @param buf_h Buffer height in pixels
 * @param origin_x Local x of the buffer's top-left pixel edge
 * @param origin_y Local y of the buffer's top-left pixel edge
 * @param scale_x Buffer pixels per local unit, x axis
 * @param scale_y Buffer pixels per local unit, y axis
 * @param rrect Rounded rectangle in local coordinates
 * @param radii Per-corner radii in local units (TL, TR, BL, BR);
 *              0 for a corner means no cut there
 */
void rounded_crop_pixels(void *pixels, size_t stride, int buf_w, int buf_h,
                         double origin_x, double origin_y,
                         double scale_x, double scale_y,
                         const struct wlr_box *rrect, const int radii[4]);

/**
 * Read a texture into a new CPU buffer (premultiplied ARGB8888) that
 * supports data pointer access. Returns NULL on failure. The caller owns
 * one reference (drop with wlr_buffer_drop()).
 */
struct wlr_buffer *rounded_crop_copy_texture(struct wlr_texture *texture);

/**
 * Copy raw CPU pixels (from wlr_buffer_begin_data_ptr_access) into a new
 * ARGB8888 buffer. ARGB8888/XRGB8888 sources only; XRGB alpha is forced
 * opaque. Returns NULL on failure. The caller owns one reference.
 */
struct wlr_buffer *rounded_crop_copy_data(int width, int height,
                                          const void *data, size_t stride,
                                          uint32_t format);

/**
 * Render a rounded border ring: a `frame_w` x `frame_h` ARGB buffer filled
 * with `color` between the outer rounded rect (per-corner `radii`) and the
 * inner rounded rect inset by `bw` (concentric, radius `radii[i] - bw` per
 * corner). Returns NULL if bw <= 0 or on failure.
 */
struct wlr_buffer *rounded_crop_render_ring(int frame_w, int frame_h, int bw,
                                            const int radii[4],
                                            const float color[4]);

/**
 * Render an inner hairline: a `content_w` x `content_h` ARGB buffer with a
 * thin stroke following the ring's inner contour (the border's inner edge).
 *
 * The stroke is centered `line_w`/2 inside the buffer edges and its leading
 * (outer) edge coincides exactly with the ring's inner rounded rect, so the
 * two contours share the same per-corner arcs (no aliasing gap). Corner
 * radii are the ring's inner radii (`max(0, radii[i] - bw)`), clamped the
 * same way the ring clamps them. Returns NULL on failure or when the line
 * does not fit the buffer.
 */
struct wlr_buffer *rounded_crop_render_innerline(int content_w, int content_h,
        int bw, const int radii[4], double line_w, const float color[4]);

/** Free owned buffers and reset the crop state (the ring scene node is
 * expected to be destroyed with its parent tree). */
void rounded_crop_release(rounded_crop_t *crop);

/* ========== Lua Integration ========== */

/**
 * Parse rounded corner configuration from Lua value.
 *
 * Accepts:
 *   - boolean: true = use defaults, false = disabled
 *   - number: uniform radius in pixels (0 = disabled, > 0 = enabled)
 *   - table:
 *       { enabled = ..., radius = N, color = ... }          uniform radius
 *       { radius = {tl, tr, bl, br} }                       per-corner radii
 *       { corner_radii = {tl, tr, bl, br} }                 per-corner radii
 *       { top_left = N, bottom_right = M, ... }             per-corner radii
 *     Any combination may be used; unspecified corners keep the current
 *     effective (default) values.
 *
 * @param L Lua state
 * @param idx Stack index of value
 * @param config Config structure to populate
 * @param is_drawin true to seed defaults from the drawin set
 * @return true if valid, false if invalid (leaves error on stack)
 */
bool rounded_config_from_lua(lua_State *L, int idx, rounded_config_t *config,
                             bool is_drawin);

/**
 * Push rounded corner configuration to Lua.
 *
 * @param L Lua state
 * @param config Config to push (NULL pushes nil)
 */
void rounded_config_to_lua(lua_State *L, const rounded_config_t *config);

/**
 * Get rounded corner defaults from beautiful theme.
 *
 * Reads beautiful.corner_* properties and updates globalconf.rounded.
 * Called during theme loading.
 *
 * @param L Lua state
 */
void rounded_load_beautiful_defaults(lua_State *L);

#endif /* SOMEWM_ROUNDED_H */