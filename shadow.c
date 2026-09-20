/*
 * shadow.c - compositor-level shadow support (nine-patch drop shadow)
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

/* The shadow is a rounded rectangle: the object's frame grown by `spread`
 * on every side, translated by (offset_x, offset_y), rounded by
 * `corner_radius`, with a `radius`-wide smoothstep falloff outside its
 * boundary. It is assembled as a nine-patch: four corner patches carry the
 * rounded falloff, four GPU-stretched edge strips carry the straight
 * falloff, and up to three solid scene rects cover the interior. Falloff
 * values agree exactly along every seam (both measure distance to the
 * shadow rectangle), so the patches meet without visible steps. */

#include "shadow.h"
#include "color.h"
#include "globalconf.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <drm_fourcc.h>

/* Default shadow configuration (disabled by default, theme enables) */
static const shadow_config_t shadow_defaults = {
    .enabled = false,
    .radius = 12,
    .offset_x = -15,
    .offset_y = -15,
    .spread = 0,
    .corner_radius = 0,
    .radii = { 0, 0, 0, 0 },
    .follow_corners = true,
    .opacity = 0.75f,
    .color = { 0.0f, 0.0f, 0.0f, 1.0f },
    .clip_directional = true,
};

/* ========== wlr_buffer Implementation ========== */

struct shadow_buffer {
    struct wlr_buffer base;
    void *data;
    int width;
    int height;
    size_t stride;
};

static void shadow_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct shadow_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    free(buffer->data);
    free(buffer);
}

static bool shadow_buffer_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
    uint32_t *format, size_t *stride)
{
    struct shadow_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    *data = buffer->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buffer->stride;
    return true;
}

static void shadow_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    /* Nothing to do */
}

static const struct wlr_buffer_impl shadow_buffer_impl = {
    .destroy = shadow_buffer_destroy,
    .begin_data_ptr_access = shadow_buffer_begin_data_ptr_access,
    .end_data_ptr_access = shadow_buffer_end_data_ptr_access,
};

/**
 * Create a wlr_buffer with given dimensions, zero-initialized.
 */
static struct wlr_buffer *
shadow_buffer_create(int width, int height)
{
    if (width <= 0 || height <= 0)
        return NULL;

    struct shadow_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;

    buffer->width = width;
    buffer->height = height;
    buffer->stride = (size_t)width * 4;

    size_t size = buffer->stride * (size_t)height;
    buffer->data = calloc(1, size);
    if (!buffer->data) {
        free(buffer);
        return NULL;
    }

    wlr_buffer_init(&buffer->base, &shadow_buffer_impl, width, height);
    return &buffer->base;
}

/* ========== Gradient Rendering ========== */

/**
 * Smoothstep falloff for shadow gradient.
 * Returns 1.0 at the shadow boundary (t=0) and 0.0 at the outer edge (t=1).
 */
static inline float
shadow_falloff(float t)
{
    if (t >= 1.0f) return 0.0f;
    if (t <= 0.0f) return 1.0f;
    float s = 1.0f - t;
    return s * s * (3.0f - 2.0f * s);
}

/**
 * Alpha for a point at signed distance sdf (pixels, positive = outside)
 * from the shadow rectangle's boundary. radius > 0 fades over that
 * distance; radius == 0 keeps a hard edge with 1px of anti-aliasing.
 */
static inline float
shadow_alpha_at(float sdf, int radius)
{
    if (radius > 0)
        return shadow_falloff(sdf / (float)radius);
    if (sdf <= -0.5f) return 1.0f;
    if (sdf >= 0.5f) return 0.0f;
    return 0.5f - sdf;
}

/**
 * Compute a premultiplied ARGB8888 pixel for the shadow color at the
 * given alpha.
 */
static inline uint32_t
shadow_pixel(const float color[4], float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    uint8_t a = (uint8_t)(alpha * 255.0f + 0.5f);
    uint8_t r = (uint8_t)(color[0] * alpha * 255.0f + 0.5f);
    uint8_t g = (uint8_t)(color[1] * alpha * 255.0f + 0.5f);
    uint8_t b = (uint8_t)(color[2] * alpha * 255.0f + 0.5f);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

/** Peak paint alpha: opacity scaled by the color's own alpha channel. */
static inline float
shadow_paint(const shadow_config_t *config)
{
    float paint = config->opacity * config->color[3];
    if (paint < 0.0f) paint = 0.0f;
    if (paint > 1.0f) paint = 1.0f;
    return paint;
}

/**
 * Render one corner patch of the shadow rectangle.
 *
 * The patch is a (radius + corner_radius) square covering the corner arc:
 * falloff outside the rounded boundary, solid inside it. The math is done
 * in top-left orientation and mirrored for the other corners.
 *
 * @param corner Corner index (0=TL, 1=TR, 2=BL, 3=BR)
 * @param radius Falloff distance
 * @param corner_radius Rounded corner radius of the shadow rect
 * @param color RGBA color
 * @param paint Peak alpha (opacity * color alpha)
 * @return wlr_buffer or NULL on failure
 */
static struct wlr_buffer *
shadow_render_corner(int corner, int radius, int corner_radius,
                     const float color[4], float paint)
{
    int side = radius + corner_radius;
    if (side <= 0)
        return NULL;

    struct wlr_buffer *wlr_buf = shadow_buffer_create(side, side);
    if (!wlr_buf)
        return NULL;

    struct shadow_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    uint32_t *pixels = (uint32_t *)buffer->data;

    bool mirror_x = (corner == 1 || corner == 3);
    bool mirror_y = (corner == 2 || corner == 3);

    /* In TL orientation the arc center sits at local (side, side): the
     * patch spans [-radius, corner_radius) from the rect corner, and the
     * center is corner_radius inside it. */
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            float lx = (mirror_x ? side - 1 - x : x) + 0.5f;
            float ly = (mirror_y ? side - 1 - y : y) + 0.5f;
            float dx = lx - (float)side;
            float dy = ly - (float)side;
            float sdf = sqrtf(dx * dx + dy * dy) - (float)corner_radius;
            pixels[y * side + x] =
                shadow_pixel(color, shadow_alpha_at(sdf, radius) * paint);
        }
    }

    return wlr_buf;
}

/**
 * Render the horizontal edge texture (1 pixel wide, radius tall).
 * Alpha peaks at row 0 (the shadow boundary) and fades to 0 at the last
 * row. Used as-is for the bottom edge; flipped 180 for the top edge.
 */
static struct wlr_buffer *
shadow_render_edge_h(int radius, const float color[4], float paint)
{
    if (radius <= 0)
        return NULL;

    struct wlr_buffer *wlr_buf = shadow_buffer_create(1, radius);
    if (!wlr_buf)
        return NULL;

    struct shadow_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    uint32_t *pixels = (uint32_t *)buffer->data;

    for (int y = 0; y < radius; y++)
        pixels[y] = shadow_pixel(color,
            shadow_alpha_at((float)y + 0.5f, radius) * paint);

    return wlr_buf;
}

/**
 * Render the vertical edge texture (radius wide, 1 pixel tall).
 * Alpha peaks at column 0. Used as-is for the right edge; flipped 180 for
 * the left edge.
 */
static struct wlr_buffer *
shadow_render_edge_v(int radius, const float color[4], float paint)
{
    if (radius <= 0)
        return NULL;

    struct wlr_buffer *wlr_buf = shadow_buffer_create(radius, 1);
    if (!wlr_buf)
        return NULL;

    struct shadow_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    uint32_t *pixels = (uint32_t *)buffer->data;

    for (int x = 0; x < radius; x++)
        pixels[x] = shadow_pixel(color,
            shadow_alpha_at((float)x + 0.5f, radius) * paint);

    return wlr_buf;
}

/* ========== Core API ========== */

void
shadow_init(void)
{
    /* Nothing to initialize - per-shadow textures are self-contained */
}

void
shadow_cleanup(void)
{
    /* Nothing to cleanup globally - per-shadow textures freed in shadow_destroy */
}

const shadow_config_t *
shadow_get_effective_config(const shadow_config_t *override, bool is_drawin)
{
    if (override)
        return override;

    return is_drawin ? &globalconf.shadow.drawin : &globalconf.shadow.client;
}

void
shadow_config_with_window_radii(shadow_config_t *out,
                                const shadow_config_t *base,
                                const int window_radii[4],
                                bool window_rounded)
{
    if (!out)
        return;

    *out = base ? *base : shadow_defaults;

    if (!out->follow_corners)
        return;

    for (int i = 0; i < 4; i++) {
        int r = 0;
        if (window_rounded && window_radii)
            r = window_radii[i] > 0 ? window_radii[i] : 0;
        out->radii[i] = r;
    }
    out->corner_radius = out->radii[SHADOW_CORNER_TL];
}

static inline int
shadow_radius(const shadow_config_t *config)
{
    return config->radius > 0 ? config->radius : 0;
}

static inline int
shadow_corner_radius(const shadow_config_t *config, int corner)
{
    int r = config->radii[corner];
    return r > 0 ? r : 0;
}

/** Largest configured corner radius (0 when every corner is square). */
static inline int
shadow_max_corner_radius(const shadow_config_t *config)
{
    int max = 0;
    for (int i = 0; i < 4; i++) {
        int r = shadow_corner_radius(config, i);
        if (r > max)
            max = r;
    }
    return max;
}

/**
 * Free owned textures in a shadow_nodes_t.
 */
static void
shadow_free_textures(shadow_nodes_t *shadow)
{
    for (int i = 0; i < SHADOW_TEXTURE_COUNT; i++) {
        if (shadow->textures[i]) {
            wlr_buffer_drop(shadow->textures[i]);
            shadow->textures[i] = NULL;
        }
    }
    if (shadow->fill_buf) {
        wlr_buffer_drop(shadow->fill_buf);
        shadow->fill_buf = NULL;
    }
}

/* The shadow is pure decoration; it must never intercept clicks. A NULL
 * point_accepts_input would make wlr_scene_node_at return it as the hit
 * (swallowing input over the frame and any window the shadow bleeds onto),
 * so reject explicitly, like the rounded ring and inner hairline. */
static bool
shadow_point_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
    (void)buffer;
    (void)sx;
    (void)sy;
    return false;
}

/**
 * Render a 1x1 solid-color texture for the interior fills.
 * Premultiplied like the gradient slices (a = paint, rgb = color*paint).
 */
static struct wlr_buffer *
shadow_render_solid(const float color[4], float paint)
{
    struct wlr_buffer *wlr_buf = shadow_buffer_create(1, 1);
    if (!wlr_buf)
        return NULL;

    struct shadow_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    ((uint32_t *)buffer->data)[0] = shadow_pixel(color, paint);

    return wlr_buf;
}

/**
 * Render gradient textures for a shadow configuration.
 * Stores results in shadow->textures:
 *   [0..3]=corners TL,TR,BL,BR, [4]=edge_h, [5]=edge_v
 * With radius and corner_radius both 0 no textures are needed (the shadow
 * is a hard rectangle drawn by the fill rects alone).
 */
static bool
shadow_render_textures(shadow_nodes_t *shadow, const shadow_config_t *config)
{
    int radius = shadow_radius(config);
    int max_corner = shadow_max_corner_radius(config);
    float paint = shadow_paint(config);

    for (int i = 0; i < 4; i++)
        shadow->textures[i] = shadow_render_corner(i, radius,
            shadow_corner_radius(config, i), config->color, paint);
    shadow->textures[4] = shadow_render_edge_h(radius, config->color, paint);
    shadow->textures[5] = shadow_render_edge_v(radius, config->color, paint);

    /* Fail on genuine allocation failure, not on legitimately empty
     * textures (radius 0 needs no edges, radius+corner_radius 0 no corners) */
    if ((radius + max_corner) > 0 && !shadow->textures[0]) {
        shadow_free_textures(shadow);
        return false;
    }

    return true;
}

bool
shadow_create(struct wlr_scene_tree *parent,
              shadow_nodes_t *shadow,
              const shadow_config_t *config,
              int width, int height)
{
    if (!parent || !shadow || !config)
        return false;

    memset(shadow, 0, sizeof(*shadow));
    shadow->fade = 1.0f;

    if (!config->enabled)
        return true;

#ifdef HAVE_SCENEFX
    /* GPU shadow: a single wlr_scene_shadow node behind the object. The
     * nine-patch slice pipeline below is skipped entirely. */
    shadow->tree = wlr_scene_tree_create(parent);
    if (!shadow->tree)
        return false;

    wlr_scene_node_lower_to_bottom(&shadow->tree->node);

    shadow->sfx_shadow = wlr_scene_shadow_create(shadow->tree,
        1, 1, 0, 0, (float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
    if (!shadow->sfx_shadow) {
        wlr_scene_node_destroy(&shadow->tree->node);
        shadow->tree = NULL;
        return false;
    }

    shadow->user_visible = true;
    shadow->size_ok = true;
    shadow->last_width = -1;
    shadow->last_height = -1;
    shadow->config = *config;
    shadow_update_geometry(shadow, config, width, height);
    return true;
#endif

    if (!shadow_render_textures(shadow, config))
        return false;

    /* Create shadow tree as first child (renders behind everything else) */
    shadow->tree = wlr_scene_tree_create(parent);
    if (!shadow->tree) {
        shadow_free_textures(shadow);
        return false;
    }

    wlr_scene_node_lower_to_bottom(&shadow->tree->node);

    for (int i = 0; i < 4; i++) {
        if (shadow->textures[i])
            shadow->slice[SHADOW_CORNER_TL + i] = wlr_scene_buffer_create(
                shadow->tree, shadow->textures[i]);
    }

    if (shadow->textures[4]) {
        shadow->slice[SHADOW_EDGE_TOP] = wlr_scene_buffer_create(
            shadow->tree, shadow->textures[4]);
        shadow->slice[SHADOW_EDGE_BOTTOM] = wlr_scene_buffer_create(
            shadow->tree, shadow->textures[4]);
    }
    if (shadow->textures[5]) {
        shadow->slice[SHADOW_EDGE_LEFT] = wlr_scene_buffer_create(
            shadow->tree, shadow->textures[5]);
        shadow->slice[SHADOW_EDGE_RIGHT] = wlr_scene_buffer_create(
            shadow->tree, shadow->textures[5]);
    }

    /* Top/left edges need a 180 flip so the opaque side touches the
     * shadow rectangle. Constant for the shadow's lifetime. */
    if (shadow->slice[SHADOW_EDGE_TOP])
        wlr_scene_buffer_set_transform(
            shadow->slice[SHADOW_EDGE_TOP], WL_OUTPUT_TRANSFORM_180);
    if (shadow->slice[SHADOW_EDGE_LEFT])
        wlr_scene_buffer_set_transform(
            shadow->slice[SHADOW_EDGE_LEFT], WL_OUTPUT_TRANSFORM_180);

    /* Make every slice input-transparent as well. */
    for (int i = 0; i < SHADOW_SLICE_COUNT; i++)
        if (shadow->slice[i])
            shadow->slice[i]->point_accepts_input = shadow_point_accepts_input;

    /* Solid interior fills (premultiplied color). Scene buffers (not rects)
     * so they get the input rejection above; a single 1x1 texture is
     * stretched to size. The side columns only exist when rounded corners
     * leave gaps beside the middle band. */
    float paint = shadow_paint(config);
    shadow->fill_buf = shadow_render_solid(config->color, paint);
    for (int i = 0; i < SHADOW_FILL_COUNT && shadow->fill_buf; i++) {
        shadow->fill[i] = wlr_scene_buffer_create(shadow->tree, shadow->fill_buf);
        if (shadow->fill[i])
            shadow->fill[i]->point_accepts_input = shadow_point_accepts_input;
    }

    /* Remember what these textures were rendered for so shadow_update() can
     * tell a pure resize from a config/rounding change. */
    shadow->config = *config;

    shadow->user_visible = true;
    shadow->size_ok = true;

    /* Ensure initial geometry update always runs (cache starts at 0,0
     * from memset, which could match a zero-sized client on first map) */
    shadow->last_width = -1;
    shadow->last_height = -1;

    shadow_update_geometry(shadow, config, width, height);

    return true;
}

/** Position a scene buffer, disabling it when it has no area. */
static void
shadow_place_slice(struct wlr_scene_buffer *slice, int x, int y, int w, int h)
{
    if (!slice)
        return;
    bool on = w > 0 && h > 0;
    wlr_scene_node_set_enabled(&slice->node, on);
    if (!on)
        return;
    wlr_scene_node_set_position(&slice->node, x, y);
    wlr_scene_buffer_set_dest_size(slice, w, h);
}

#ifdef HAVE_SCENEFX
/**
 * Geometry update for the SceneFX GPU shadow.
 *
 * The shader renders the shadow of a rounded rect that is the shadow node's
 * box inset by blur_sigma on every side (box_shadow.frag), so the node is
 * sized to the object footprint grown by the falloff and offset by it. SceneFX
 * shadows take a single corner radius, so the widest of the four configured
 * corner radii is used. opacity is baked into the color's alpha channel.
 */
static void
shadow_sfx_update_geometry(shadow_nodes_t *shadow,
                           const shadow_config_t *config,
                           int width, int height)
{
    /* The nine-patch's authoritative shadow rect is the object grown by
     * `spread` and translated by (offset_x, offset_y); the shader derives it
     * as the node box inset by blur_sigma, so grow the node by 2*spread and
     * shift it by -spread to reproduce the same box. */
    int radius = shadow_radius(config);
    int w = width + 2 * config->spread + 2 * radius;
    int h = height + 2 * config->spread + 2 * radius;
    int x = config->offset_x - config->spread - radius;
    int y = config->offset_y - config->spread - radius;
    float paint = shadow_paint(config) * shadow->fade;

    struct wlr_scene_shadow *sfx = shadow->sfx_shadow;
    wlr_scene_shadow_set_size(sfx, w, h);
    wlr_scene_shadow_set_corner_radius(sfx,
        shadow_max_corner_radius(config));
    wlr_scene_shadow_set_blur_sigma(sfx, radius);
    float color[4] = { config->color[0], config->color[1],
                       config->color[2], paint };
    wlr_scene_shadow_set_color(sfx, color);
    wlr_scene_node_set_position(&sfx->node, x, y);
    wlr_scene_node_set_enabled(&sfx->node,
        shadow->user_visible && shadow->size_ok);
}
#endif

void
shadow_update_geometry(shadow_nodes_t *shadow,
                      const shadow_config_t *config,
                      int width, int height)
{
    if (!shadow || !shadow->tree || !config)
        return;

    /* Skip if geometry hasn't changed - avoids redundant damage.
     * Config changes go through shadow_update_config() which does
     * destroy+create, resetting the cache via memset. */
    if (shadow->last_width == width && shadow->last_height == height)
        return;
    shadow->last_width = width;
    shadow->last_height = height;

#ifdef HAVE_SCENEFX
    if (shadow->sfx_shadow) {
        shadow_sfx_update_geometry(shadow, config, width, height);
        return;
    }
#endif

    int radius = shadow_radius(config);
    int rtl = shadow_corner_radius(config, SHADOW_CORNER_TL);
    int rtr = shadow_corner_radius(config, SHADOW_CORNER_TR);
    int rbl = shadow_corner_radius(config, SHADOW_CORNER_BL);
    int rbr = shadow_corner_radius(config, SHADOW_CORNER_BR);
    int sw = width + 2 * config->spread;
    int sh = height + 2 * config->spread;
    int bx = config->offset_x - config->spread;
    int by = config->offset_y - config->spread;

    /* The arcs must fit without overlapping: each horizontal edge needs room
     * for the two radii meeting there, and likewise each vertical edge.
     * Otherwise hide the shadow rather than let the patches overlap. */
    shadow->size_ok = sw > 0 && sh > 0
        && sw >= rtl + rtr && sw >= rbl + rbr
        && sh >= rtl + rbl && sh >= rtr + rbr;
    wlr_scene_node_set_enabled(&shadow->tree->node,
        shadow->user_visible && shadow->size_ok);
    if (!shadow->size_ok)
        return;

    /* Corner patches: (radius + corner radius) squares, each centred on its
     * own arc, so the four corners may have different radii. */
    shadow_place_slice(shadow->slice[SHADOW_CORNER_TL],
        bx - radius, by - radius, radius + rtl, radius + rtl);
    shadow_place_slice(shadow->slice[SHADOW_CORNER_TR],
        bx + sw - rtr, by - radius, radius + rtr, radius + rtr);
    shadow_place_slice(shadow->slice[SHADOW_CORNER_BL],
        bx - radius, by + sh - rbl, radius + rbl, radius + rbl);
    shadow_place_slice(shadow->slice[SHADOW_CORNER_BR],
        bx + sw - rbr, by + sh - rbr, radius + rbr, radius + rbr);

    /* Edge strips span between the two corner patches they connect. */
    shadow_place_slice(shadow->slice[SHADOW_EDGE_TOP],
        bx + rtl, by - radius, sw - rtl - rtr, radius);
    shadow_place_slice(shadow->slice[SHADOW_EDGE_BOTTOM],
        bx + rbl, by + sh, sw - rbl - rbr, radius);
    shadow_place_slice(shadow->slice[SHADOW_EDGE_LEFT],
        bx - radius, by + rtl, radius, sh - rtl - rbl);
    shadow_place_slice(shadow->slice[SHADOW_EDGE_RIGHT],
        bx + sw, by + rtr, radius, sh - rtr - rbr);

    /* Interior: a full-height central band, two side columns, and four caps
     * that step out to the wider of the two radii on each side. Non-
     * overlapping, so a semi-transparent shadow never double-blends. */
    int lx = rtl > rbl ? rtl : rbl;   /* left column width */
    int rx = rtr > rbr ? rtr : rbr;   /* right column width */

    shadow_place_slice(shadow->fill[SHADOW_FILL_MID],
        bx + lx, by, sw - lx - rx, sh);
    shadow_place_slice(shadow->fill[SHADOW_FILL_LEFT],
        bx, by + rtl, lx, sh - rtl - rbl);
    shadow_place_slice(shadow->fill[SHADOW_FILL_RIGHT],
        bx + sw - rx, by + rtr, rx, sh - rtr - rbr);
    shadow_place_slice(shadow->fill[SHADOW_FILL_LEFT_TOP],
        bx + rtl, by, lx - rtl, rtl);
    shadow_place_slice(shadow->fill[SHADOW_FILL_LEFT_BOTTOM],
        bx + rbl, by + sh - rbl, lx - rbl, rbl);
    shadow_place_slice(shadow->fill[SHADOW_FILL_RIGHT_TOP],
        bx + sw - rx, by, rx - rtr, rtr);
    shadow_place_slice(shadow->fill[SHADOW_FILL_RIGHT_BOTTOM],
        bx + sw - rx, by + sh - rbr, rx - rbr, rbr);
}

void
shadow_update_config(shadow_nodes_t *shadow,
                    struct wlr_scene_tree *parent,
                    const shadow_config_t *config,
                    int width, int height)
{
    float fade;

    if (!shadow || !config)
        return;

    /* Destroy existing shadow and recreate with new config.
     * Gradient textures are tiny so recreation is cheap. A config change
     * mid-fade (e.g. focus flip) must not restore a full shadow; a struct
     * that never rendered has no meaningful fade yet. */
    fade = shadow->tree ? shadow->fade : 1.0f;
    shadow_destroy(shadow);

    if (config->enabled)
        shadow_create(parent, shadow, config, width, height);
    shadow_set_fade(shadow, fade);
}

/* True when two resolved configs would render identical textures. Geometry
 * (offsets/spread/size) is handled separately by shadow_update_geometry. */
static bool
shadow_config_equal(const shadow_config_t *a, const shadow_config_t *b)
{
    if (a->enabled != b->enabled || a->radius != b->radius)
        return false;
    for (int i = 0; i < 4; i++) {
        if (a->radii[i] != b->radii[i])
            return false;
    }
    if (a->opacity != b->opacity)
        return false;
    for (int i = 0; i < 4; i++) {
        if (a->color[i] != b->color[i])
            return false;
    }
    return true;
}

void
shadow_update(shadow_nodes_t *shadow,
              struct wlr_scene_tree *parent,
              const shadow_config_t *config,
              int width, int height)
{
    if (!shadow || !config)
        return;

    if (shadow->tree && shadow_config_equal(&shadow->config, config)) {
        shadow_update_geometry(shadow, config, width, height);
        return;
    }

    shadow_update_config(shadow, parent, config, width, height);
}

void
shadow_set_visible(shadow_nodes_t *shadow, bool visible)
{
    if (!shadow)
        return;

    shadow->user_visible = visible;
    if (shadow->tree)
        wlr_scene_node_set_enabled(&shadow->tree->node,
            visible && shadow->size_ok);
}

void
shadow_set_fade(shadow_nodes_t *shadow, float fade)
{
    if (!shadow)
        return;
    if (fade < 0.0f)
        fade = 0.0f;
    if (fade > 1.0f)
        fade = 1.0f;
    shadow->fade = fade;

#ifdef HAVE_SCENEFX
    if (shadow->sfx_shadow) {
        float paint = shadow_paint(&shadow->config) * fade;
        float color[4] = { shadow->config.color[0],
                           shadow->config.color[1],
                           shadow->config.color[2], paint };
        wlr_scene_shadow_set_color(shadow->sfx_shadow, color);
        return;
    }
#endif
    /* Nine-patch: gradient textures carry their own alpha, attenuate the
     * slice/fill buffers on top. */
    if (shadow->tree)
        scene_apply_opacity(&shadow->tree->node, fade);
}

void
shadow_destroy(shadow_nodes_t *shadow)
{
    if (!shadow)
        return;

    if (shadow->tree)
        wlr_scene_node_destroy(&shadow->tree->node);

    shadow_release(shadow);
}

void
shadow_release(shadow_nodes_t *shadow)
{
    if (!shadow)
        return;

    shadow_free_textures(shadow);
    memset(shadow, 0, sizeof(*shadow));
}

/* ========== Lua Integration ========== */

/** Per-corner Lua keys, ordered TL, TR, BL, BR. */
static const char *const shadow_corner_keys[4] = {
    "top_left", "top_right", "bottom_left", "bottom_right"
};

/**
 * Parse a radius value ({@code number} or table) into per-corner radii.
 * A plain number sets all four corners; a positional table {tl,tr,bl,br}
 * and/or named keys set individual corners.
 */
static void
shadow_parse_radii_value(lua_State *L, int idx, int radii[4])
{
    if (lua_isnumber(L, idx)) {
        int n = (int)lua_tointeger(L, idx);
        for (int i = 0; i < 4; i++)
            radii[i] = n;
    } else if (lua_istable(L, idx)) {
        /* Positional {tl, tr, bl, br}? */
        lua_rawgeti(L, idx, 1);
        bool positional = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (positional) {
            for (int i = 0; i < 4; i++) {
                lua_rawgeti(L, idx, i + 1);
                if (lua_isnumber(L, -1))
                    radii[i] = (int)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
        /* `radius` / `corner_radii` wrapper keys (number or table) */
        lua_getfield(L, idx, "radius");
        if (!lua_isnil(L, -1))
            shadow_parse_radii_value(L, -1, radii);
        lua_pop(L, 1);
        lua_getfield(L, idx, "corner_radii");
        if (!lua_isnil(L, -1))
            shadow_parse_radii_value(L, -1, radii);
        lua_pop(L, 1);
        /* Named per-corner keys at this level */
        for (int i = 0; i < 4; i++) {
            lua_getfield(L, idx, shadow_corner_keys[i]);
            if (lua_isnumber(L, -1))
                radii[i] = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    }
}

bool
shadow_config_from_lua(lua_State *L, int idx, shadow_config_t *config,
                       bool is_drawin)
{
    if (!config)
        return false;

    /* Start from theme defaults (not hardcoded defaults) */
    *config = *shadow_get_effective_config(NULL, is_drawin);

    if (lua_isboolean(L, idx)) {
        config->enabled = lua_toboolean(L, idx);
        return true;
    }

    if (lua_isnil(L, idx)) {
        config->enabled = false;
        return true;
    }

    if (!lua_istable(L, idx)) {
        lua_pushstring(L, "shadow must be boolean or table");
        return false;
    }

    /* Parse table fields */
    config->enabled = true;

    lua_getfield(L, idx, "enabled");
    if (!lua_isnil(L, -1))
        config->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "radius");
    if (lua_isnumber(L, -1))
        config->radius = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "offset_x");
    if (lua_isnumber(L, -1))
        config->offset_x = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "offset_y");
    if (lua_isnumber(L, -1))
        config->offset_y = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "spread");
    if (lua_isnumber(L, -1))
        config->spread = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    /* corner_radius = uniform number or per-corner table */
    bool explicit_radii = false;
    lua_getfield(L, idx, "corner_radius");
    if (!lua_isnil(L, -1)) {
        shadow_parse_radii_value(L, -1, config->radii);
        explicit_radii = true;
    }
    lua_pop(L, 1);

    /* `radii` / `corner_radii` = positional {tl, tr, bl, br} */
    lua_getfield(L, idx, "radii");
    if (!lua_isnil(L, -1)) {
        shadow_parse_radii_value(L, -1, config->radii);
        explicit_radii = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "corner_radii");
    if (!lua_isnil(L, -1)) {
        shadow_parse_radii_value(L, -1, config->radii);
        explicit_radii = true;
    }
    lua_pop(L, 1);

    /* Named per-corner keys at table level */
    for (int i = 0; i < 4; i++) {
        lua_getfield(L, idx, shadow_corner_keys[i]);
        if (lua_isnumber(L, -1)) {
            config->radii[i] = (int)lua_tointeger(L, -1);
            explicit_radii = true;
        }
        lua_pop(L, 1);
    }

    /* Following the window's rounding is the default; naming explicit radii
     * opts out so a hand-tuned shadow stays hand-tuned. */
    lua_getfield(L, idx, "follow_corners");
    if (!lua_isnil(L, -1))
        config->follow_corners = lua_toboolean(L, -1);
    else if (explicit_radii)
        config->follow_corners = false;
    lua_pop(L, 1);

    config->corner_radius = config->radii[SHADOW_CORNER_TL];

    lua_getfield(L, idx, "opacity");
    if (lua_isnumber(L, -1))
        config->opacity = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "clip_directional");
    if (!lua_isnil(L, -1))
        config->clip_directional = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "color");
    if (!lua_isnil(L, -1)) {
        if (lua_isstring(L, -1)) {
            const char *str = lua_tostring(L, -1);
            color_t c;
            if (color_init_from_string(&c, str)) {
                config->color[0] = c.red / 255.0f;
                config->color[1] = c.green / 255.0f;
                config->color[2] = c.blue / 255.0f;
                config->color[3] = c.alpha / 255.0f;
            }
        } else if (lua_istable(L, -1)) {
            for (int i = 0; i < 4; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_isnumber(L, -1))
                    config->color[i] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1);

    return true;
}

void
shadow_config_to_lua(lua_State *L, const shadow_config_t *config)
{
    if (!config) {
        lua_pushnil(L);
        return;
    }

    if (!config->enabled) {
        lua_pushboolean(L, false);
        return;
    }

    lua_newtable(L);

    lua_pushboolean(L, config->enabled);
    lua_setfield(L, -2, "enabled");

    lua_pushinteger(L, config->radius);
    lua_setfield(L, -2, "radius");

    lua_pushinteger(L, config->offset_x);
    lua_setfield(L, -2, "offset_x");

    lua_pushinteger(L, config->offset_y);
    lua_setfield(L, -2, "offset_y");

    lua_pushinteger(L, config->spread);
    lua_setfield(L, -2, "spread");

    lua_pushboolean(L, config->follow_corners);
    lua_setfield(L, -2, "follow_corners");

    bool uniform = true;
    for (int i = 1; i < 4; i++)
        uniform = uniform && config->radii[i] == config->radii[0];
    if (!uniform) {
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; i++) {
            lua_pushinteger(L, config->radii[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "corner_radii");
    }
    lua_pushinteger(L, config->radii[SHADOW_CORNER_TL]);
    lua_setfield(L, -2, "corner_radius");

    lua_pushnumber(L, config->opacity);
    lua_setfield(L, -2, "opacity");

    lua_pushboolean(L, config->clip_directional);
    lua_setfield(L, -2, "clip_directional");

    /* Color as hex string; include the alpha byte when it carries data */
    char color_str[11];
    if (config->color[3] < 1.0f)
        snprintf(color_str, sizeof(color_str), "#%02X%02X%02X%02X",
                 (int)(config->color[0] * 255),
                 (int)(config->color[1] * 255),
                 (int)(config->color[2] * 255),
                 (int)(config->color[3] * 255 + 0.5f));
    else
        snprintf(color_str, sizeof(color_str), "#%02X%02X%02X",
                 (int)(config->color[0] * 255),
                 (int)(config->color[1] * 255),
                 (int)(config->color[2] * 255));
    lua_pushstring(L, color_str);
    lua_setfield(L, -2, "color");
}

/** Read one integer beautiful key into an int field if set. */
static void
shadow_beautiful_int(lua_State *L, const char *key, int *out)
{
    lua_getfield(L, -1, key);
    if (lua_isnumber(L, -1))
        *out = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
}

/** Read one radius beautiful key (number or {tl,tr,bl,br}) into radii. */
static void
shadow_beautiful_radius(lua_State *L, const char *key, int radii[4])
{
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1))
        shadow_parse_radii_value(L, -1, radii);
    lua_pop(L, 1);
}

/** Read one color beautiful key into a float[4] if set and valid. */
static void
shadow_beautiful_color(lua_State *L, const char *key, float out[4])
{
    lua_getfield(L, -1, key);
    if (lua_isstring(L, -1)) {
        const char *str = lua_tostring(L, -1);
        color_t c;
        if (color_init_from_string(&c, str)) {
            out[0] = c.red / 255.0f;
            out[1] = c.green / 255.0f;
            out[2] = c.blue / 255.0f;
            out[3] = c.alpha / 255.0f;
        }
    }
    lua_pop(L, 1);
}

void
shadow_load_beautiful_defaults(lua_State *L)
{
    /* Use require() to get beautiful module (it's typically local, not global) */
    lua_getglobal(L, "require");
    lua_pushstring(L, "beautiful");
    if (lua_pcall(L, 1, 1, 0) != 0) {
        lua_pop(L, 1);
        return;
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    /* Reset to defaults before parsing */
    globalconf.shadow.client = shadow_defaults;
    globalconf.shadow.drawin = shadow_defaults;

    shadow_config_t *client = &globalconf.shadow.client;

    /* Client shadow defaults */
    lua_getfield(L, -1, "shadow_enabled");
    if (!lua_isnil(L, -1))
        client->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    shadow_beautiful_int(L, "shadow_radius", &client->radius);
    shadow_beautiful_int(L, "shadow_offset_x", &client->offset_x);
    shadow_beautiful_int(L, "shadow_offset_y", &client->offset_y);
    shadow_beautiful_int(L, "shadow_spread", &client->spread);
    shadow_beautiful_radius(L, "shadow_corner_radius", client->radii);
    client->corner_radius = client->radii[SHADOW_CORNER_TL];

    lua_getfield(L, -1, "shadow_follow_corners");
    if (!lua_isnil(L, -1))
        client->follow_corners = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "shadow_opacity");
    if (lua_isnumber(L, -1))
        client->opacity = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "shadow_clip");
    if (!lua_isnil(L, -1)) {
        if (lua_isboolean(L, -1)) {
            client->clip_directional = lua_toboolean(L, -1);
        } else if (lua_isstring(L, -1)) {
            const char *clip = lua_tostring(L, -1);
            if (clip)
                client->clip_directional =
                    (strcmp(clip, "directional") == 0);
        }
    }
    lua_pop(L, 1);

    shadow_beautiful_color(L, "shadow_color", client->color);

    /* Copy client defaults to drawin, then apply drawin-specific overrides */
    globalconf.shadow.drawin = *client;
    shadow_config_t *drawin = &globalconf.shadow.drawin;

    lua_getfield(L, -1, "shadow_drawin_enabled");
    if (!lua_isnil(L, -1))
        drawin->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    shadow_beautiful_int(L, "shadow_drawin_radius", &drawin->radius);
    shadow_beautiful_int(L, "shadow_drawin_offset_x", &drawin->offset_x);
    shadow_beautiful_int(L, "shadow_drawin_offset_y", &drawin->offset_y);
    shadow_beautiful_int(L, "shadow_drawin_spread", &drawin->spread);
    shadow_beautiful_radius(L, "shadow_drawin_corner_radius", drawin->radii);
    drawin->corner_radius = drawin->radii[SHADOW_CORNER_TL];

    lua_getfield(L, -1, "shadow_drawin_follow_corners");
    if (!lua_isnil(L, -1))
        drawin->follow_corners = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "shadow_drawin_opacity");
    if (lua_isnumber(L, -1))
        drawin->opacity = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    shadow_beautiful_color(L, "shadow_drawin_color", drawin->color);

    lua_pop(L, 1);  /* Pop beautiful table */
}

/* vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80 */
