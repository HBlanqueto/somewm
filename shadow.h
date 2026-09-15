/*
 * shadow.h - compositor-level shadow support
 *
 * Copyright © 2025 somewm contributors
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

#ifndef SOMEWM_SHADOW_H
#define SOMEWM_SHADOW_H

#include <lua.h>
#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>

/**
 * Gradient slice indices for the shadow nine-patch.
 *
 * The shadow is a rounded rectangle: the object's frame grown by `spread`
 * on every side, translated by the offset, with a `radius`-wide falloff
 * outside its boundary. Corner patches carry the rounded falloff, edge
 * strips carry the straight falloff, and solid rects (see the fill enum)
 * cover the interior.
 *
 *   TL  TOP  TR
 *   L  (fill) R
 *   BL  BOT  BR
 */
enum {
    SHADOW_CORNER_TL = 0,
    SHADOW_CORNER_TR,
    SHADOW_CORNER_BL,
    SHADOW_CORNER_BR,
    SHADOW_EDGE_TOP,
    SHADOW_EDGE_BOTTOM,
    SHADOW_EDGE_LEFT,
    SHADOW_EDGE_RIGHT,
    SHADOW_SLICE_COUNT
};

/**
 * Solid interior rects. The corner patches own the four corner squares of
 * the shadow rectangle; these rects cover the rest of the interior.
 *
 * With per-corner radii the left and right columns do not reach the top or
 * bottom edge uniformly, so four small "cap" rects fill the steps above and
 * below them. The decomposition is non-overlapping so semi-transparent
 * shadows never double-blend where two rects meet.
 */
enum {
    SHADOW_FILL_MID = 0,     /**< Full-height band between the corner columns */
    SHADOW_FILL_LEFT,        /**< Left column between the two left corners */
    SHADOW_FILL_RIGHT,       /**< Right column between the two right corners */
    SHADOW_FILL_LEFT_TOP,    /**< Above the left column, right of the TL arc */
    SHADOW_FILL_LEFT_BOTTOM, /**< Below the left column, right of the BL arc */
    SHADOW_FILL_RIGHT_TOP,   /**< Above the right column, left of the TR arc */
    SHADOW_FILL_RIGHT_BOTTOM,/**< Below the right column, left of the BR arc */
    SHADOW_FILL_COUNT
};

/** Number of owned texture buffers (4 corners + h edge + v edge) */
#define SHADOW_TEXTURE_COUNT 6

/**
 * Shadow configuration for a single object (client or drawin).
 *
 * When NULL on an object, global defaults from globalconf are used.
 * When non-NULL, these values override the defaults.
 */
typedef struct shadow_config_t {
    bool enabled;           /**< Shadow enabled for this object */
    int radius;             /**< Falloff distance in pixels (default: 12) */
    int offset_x;           /**< Horizontal offset (default: -15) */
    int offset_y;           /**< Vertical offset (default: -15) */
    int spread;             /**< Outset of the shadow rect before falloff (default: 0) */
    int corner_radius;      /**< Uniform corner radius shorthand (mirrors radii[TL]) */
    int radii[4];           /**< Per-corner radius (TL, TR, BL, BR; 0 = square) */
    bool follow_corners;    /**< Adopt the object's rounded-corner radii (default: true) */
    float opacity;          /**< Shadow opacity 0.0-1.0 (default: 0.75) */
    float color[4];         /**< Shadow color RGBA; alpha multiplies opacity */
    bool clip_directional;  /**< Accepted for compatibility; no longer used */
} shadow_config_t;

/** Set all four shadow corners to the same radius. */
static inline void
shadow_config_set_uniform(shadow_config_t *cfg, int radius)
{
    cfg->corner_radius = radius;
    cfg->radii[0] = radius;
    cfg->radii[1] = radius;
    cfg->radii[2] = radius;
    cfg->radii[3] = radius;
}

/**
 * Shadow scene nodes attached to a client or drawin.
 *
 * Each shadow owns its own set of gradient textures. The slice scene
 * buffers and solid fill rects are arranged in a nine-patch; edges are
 * stretched by the GPU via dest_size, so a resize never re-renders.
 */
typedef struct shadow_nodes_t {
    struct wlr_scene_tree *tree;                        /**< Container for shadow slices */
    struct wlr_scene_buffer *slice[SHADOW_SLICE_COUNT]; /**< Gradient scene buffers */
    struct wlr_scene_rect *fill[SHADOW_FILL_COUNT];     /**< Solid interior rects */
    struct wlr_buffer *textures[SHADOW_TEXTURE_COUNT];  /**< Owned gradient textures */
    int last_width;                                     /**< Cached width to skip redundant updates */
    int last_height;                                    /**< Cached height to skip redundant updates */
    shadow_config_t config;                             /**< Config the textures were rendered for */
    bool user_visible;                                  /**< Visibility requested via shadow_set_visible */
    bool size_ok;                                       /**< Object large enough for the corner patches */
} shadow_nodes_t;

/**
 * Global shadow defaults (stored in globalconf.shadow).
 */
typedef struct shadow_defaults_t {
    shadow_config_t client;   /**< Default for clients */
    shadow_config_t drawin;   /**< Default for drawins/wiboxes */
} shadow_defaults_t;

/* ========== Core API ========== */

/**
 * Initialize shadow subsystem.
 * Call once at compositor startup.
 */
void shadow_init(void);

/**
 * Cleanup shadow subsystem.
 * Call at compositor shutdown.
 */
void shadow_cleanup(void);

/**
 * Get effective shadow configuration for an object.
 *
 * @param override Object-specific config (may be NULL for defaults)
 * @param is_drawin true for drawin, false for client
 * @return Effective configuration (never NULL)
 */
const shadow_config_t *shadow_get_effective_config(
    const shadow_config_t *override, bool is_drawin);

/**
 * Resolve the config a shadow should actually render with.
 *
 * Copies `base` into `out` and, when `base->follow_corners` is set, replaces
 * the per-corner radii with the object's effective rounded-corner radii so
 * the shadow follows the window shape (including per-corner differences).
 * When `window_rounded` is false the shadow gets square corners instead.
 *
 * @param out            Destination config
 * @param base           Object/theme config (may be NULL -> defaults)
 * @param window_radii   Effective window radii (TL, TR, BL, BR); may be NULL
 * @param window_rounded Whether the object is actually rounded
 */
void shadow_config_with_window_radii(shadow_config_t *out,
                                     const shadow_config_t *base,
                                     const int window_radii[4],
                                     bool window_rounded);

/* ========== Shadow Rendering ========== */

/**
 * Create shadow nodes for an object.
 *
 * Renders gradient textures and creates the nine-patch scene nodes as
 * children of the given parent tree, positioned below (behind) other
 * content.
 *
 * @param parent Parent scene tree (client->scene or drawin->scene_tree)
 * @param shadow Shadow nodes structure to populate
 * @param config Shadow configuration to use
 * @param width Object width in pixels
 * @param height Object height in pixels
 * @return true on success, false on failure
 */
bool shadow_create(struct wlr_scene_tree *parent,
                   shadow_nodes_t *shadow,
                   const shadow_config_t *config,
                   int width, int height);

/**
 * Update shadow geometry after object resize.
 *
 * Fast operation: just repositions scene nodes and updates dest_size.
 * No texture re-rendering.
 *
 * @param shadow Shadow nodes structure
 * @param config Shadow configuration
 * @param width New object width
 * @param height New object height
 */
void shadow_update_geometry(shadow_nodes_t *shadow,
                           const shadow_config_t *config,
                           int width, int height);

/**
 * Update a shadow from its resolved config, recreating the gradient
 * textures only when the config changed. Cheap when only the size moved.
 *
 * @param shadow Shadow nodes structure
 * @param parent Parent scene tree (for recreation)
 * @param config Resolved configuration
 * @param width Object width
 * @param height Object height
 */
void shadow_update(shadow_nodes_t *shadow,
                   struct wlr_scene_tree *parent,
                   const shadow_config_t *config,
                   int width, int height);

/**
 * Update shadow after configuration change.
 *
 * Destroys and recreates the shadow with new textures.
 *
 * @param shadow Shadow nodes structure
 * @param parent Parent scene tree (for recreation)
 * @param config New configuration
 * @param width Object width
 * @param height Object height
 */
void shadow_update_config(shadow_nodes_t *shadow,
                         struct wlr_scene_tree *parent,
                         const shadow_config_t *config,
                         int width, int height);

/**
 * Show or hide shadow.
 *
 * @param shadow Shadow nodes structure
 * @param visible true to show, false to hide
 */
void shadow_set_visible(shadow_nodes_t *shadow, bool visible);

/**
 * Destroy shadow nodes and free owned textures.
 *
 * @param shadow Shadow nodes structure to cleanup
 */
void shadow_destroy(shadow_nodes_t *shadow);

/**
 * Free owned textures and zero the structure WITHOUT destroying the scene
 * nodes. For teardown paths where the parent scene tree has been (or is
 * about to be) destroyed, taking the shadow nodes with it.
 *
 * @param shadow Shadow nodes structure to release
 */
void shadow_release(shadow_nodes_t *shadow);

/* ========== Lua Integration ========== */

/**
 * Parse shadow configuration from Lua value.
 *
 * Accepts:
 *   - boolean: true = use defaults, false = disabled
 *   - table: { radius = N, offset_x = N, ... }
 *
 * @param L Lua state
 * @param idx Stack index of value
 * @param config Config structure to populate
 * @return true if valid, false if invalid (leaves error on stack)
 */
bool shadow_config_from_lua(lua_State *L, int idx, shadow_config_t *config,
                           bool is_drawin);

/**
 * Push shadow configuration to Lua.
 *
 * @param L Lua state
 * @param config Config to push (NULL pushes nil)
 */
void shadow_config_to_lua(lua_State *L, const shadow_config_t *config);

/**
 * Get shadow defaults from beautiful theme.
 *
 * Reads beautiful.shadow_* properties and updates globalconf.shadow.
 * Called during theme loading.
 *
 * @param L Lua state
 */
void shadow_load_beautiful_defaults(lua_State *L);

#endif /* SOMEWM_SHADOW_H */
