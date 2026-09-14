/*
 * rounded.c - compositor-level rounded corner support
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

/* Four anti-aliased quarter-circle masks, one per corner of the object's
 * frame. Each mask square is `radius` x `radius`: pixels outside the
 * quarter-circle (the cut corner) are painted the configured `color`,
 * pixels inside it stay transparent so the object's own content shows
 * through. Overlaid above the object (mirroring how the shadow overlays
 * below it), this produces rounded corners whose cut area reveals the
 * background color. wlroots 0.20's scene API has no rounded clip, so the
 * masks are the scene-based equivalent. */

#include "rounded.h"
#include "color.h"
#include "globalconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>

/* Default rounded configuration (disabled by default, theme enables) */
static const rounded_config_t rounded_defaults = {
    .enabled = false,
    .radius = 0,
    .radii = { 0, 0, 0, 0 },
    .color = { 0.0f, 0.0f, 0.0f, 1.0f },
};

/* ========== wlr_buffer Implementation ========== */

struct rounded_buffer {
    struct wlr_buffer base;
    void *data;
    int width;
    int height;
    size_t stride;
};

static void
rounded_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct rounded_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    free(buffer->data);
    free(buffer);
}

static bool
rounded_buffer_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
    uint32_t *format, size_t *stride)
{
    struct rounded_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    *data = buffer->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buffer->stride;
    return true;
}

static void
rounded_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    /* Nothing to do */
}

static const struct wlr_buffer_impl rounded_buffer_impl = {
    .destroy = rounded_buffer_destroy,
    .begin_data_ptr_access = rounded_buffer_begin_data_ptr_access,
    .end_data_ptr_access = rounded_buffer_end_data_ptr_access,
};

/**
 * Create a wlr_buffer with given dimensions, zero-initialized.
 */
static struct wlr_buffer *
rounded_buffer_create(int width, int height)
{
    if (width <= 0 || height <= 0)
        return NULL;

    struct rounded_buffer *buffer = calloc(1, sizeof(*buffer));
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

    wlr_buffer_init(&buffer->base, &rounded_buffer_impl, width, height);
    return &buffer->base;
}

/* ========== Corner Mask Rendering ========== */

/**
 * Coverage alpha for a quarter-circle corner cut.
 *
 * For a corner mask square of side `r`, the rounded rectangle's arc is a
 * quarter-circle of radius `r` centered at the inner corner of the square
 * (local coordinate (r, r)). Points at distance d from that center:
 *   - d <= r: inside the rounded rectangle -> keep the object (alpha 0)
 *   - d >  r: the cut corner -> painted over (alpha 1)
 * The boundary is anti-aliased over 1px.
 */
static inline float
rounded_cut_alpha(float d, float r)
{
    if (d <= r - 0.5f)
        return 0.0f;
    if (d >= r + 0.5f)
        return 1.0f;
    /* Smoothstep over the 1px transition band. */
    float t = (d - (r - 0.5f)) / 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/**
 * Compute a premultiplied ARGB8888 pixel for the mask color at the
 * given alpha.
 */
static inline uint32_t
rounded_pixel(const float color[4], float alpha)
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

/**
 * Render one corner mask square.
 *
 * The square is a `radius` x `radius` patch covering the object's corner.
 * The math is done in top-left orientation and mirrored for the other
 * corners: the arc center sits at local (side, side) and pixels whose
 * distance from it exceeds `radius` are the cut corner.
 *
 * @param corner Corner index (0=TL, 1=TR, 2=BL, 3=BR)
 * @param radius Corner radius of the rounded rectangle
 * @param color RGBA color painted in the cut corner
 * @return wlr_buffer or NULL on failure
 */
static struct wlr_buffer *
rounded_render_corner(int corner, int radius, const float color[4])
{
    int side = radius;
    if (side <= 0)
        return NULL;

    struct wlr_buffer *wlr_buf = rounded_buffer_create(side, side);
    if (!wlr_buf)
        return NULL;

    struct rounded_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    uint32_t *pixels = (uint32_t *)buffer->data;

    bool mirror_x = (corner == 1 || corner == 3);
    bool mirror_y = (corner == 2 || corner == 3);

    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            float lx = (mirror_x ? side - 1 - x : x) + 0.5f;
            float ly = (mirror_y ? side - 1 - y : y) + 0.5f;
            float dx = lx - (float)side;
            float dy = ly - (float)side;
            float d = sqrtf(dx * dx + dy * dy);
            float alpha = rounded_cut_alpha(d, (float)side) * color[3];
            pixels[y * side + x] = rounded_pixel(color, alpha);
        }
    }

    return wlr_buf;
}

/* ========== Core API ========== */

const rounded_config_t *
rounded_get_effective_config(const rounded_config_t *override, bool is_drawin)
{
    if (override)
        return override;

    return is_drawin ? &globalconf.rounded.drawin : &globalconf.rounded.client;
}

/**
 * Free owned textures in a rounded_nodes_t.
 */
static void
rounded_free_textures(rounded_nodes_t *rounded)
{
    for (int i = 0; i < ROUNDED_TEXTURE_COUNT; i++) {
        if (rounded->textures[i]) {
            wlr_buffer_drop(rounded->textures[i]);
            rounded->textures[i] = NULL;
        }
    }
}

/**
 * Render corner mask textures for a rounded configuration.
 * Stores results in rounded->textures:
 *   [0..3] = corners TL,TR,BL,BR
 * A corner with radius 0 needs no texture (stays NULL and is not placed).
 */
static bool
rounded_render_textures(rounded_nodes_t *rounded, const rounded_config_t *config)
{
    bool any = rounded_config_active(config);

    for (int i = 0; i < 4; i++)
        rounded->textures[i] = rounded_render_corner(i, config->radii[i],
                                                     config->color);

    /* Fail on genuine allocation failure, not on legitimately empty
     * textures (a corner with radius 0 needs no mask) */
    if (any && !rounded->textures[0] && !rounded->textures[1]
            && !rounded->textures[2] && !rounded->textures[3]) {
        rounded_free_textures(rounded);
        return false;
    }

    return true;
}

bool
rounded_create(struct wlr_scene_tree *parent,
               rounded_nodes_t *rounded,
               const rounded_config_t *config,
               int width, int height)
{
    if (!parent || !rounded || !config)
        return false;

    memset(rounded, 0, sizeof(*rounded));

    if (!rounded_config_active(config))
        return true;

    if (!rounded_render_textures(rounded, config))
        return false;

    /* Create the mask tree as the last child so it draws over the object's
     * content, borders and titlebars. Popups are raised above it by the
     * geometry pass (see window.c apply_geometry_to_wlroots). */
    rounded->tree = wlr_scene_tree_create(parent);
    if (!rounded->tree) {
        rounded_free_textures(rounded);
        return false;
    }

    wlr_scene_node_raise_to_top(&rounded->tree->node);

    for (int i = 0; i < 4; i++) {
        if (rounded->textures[i])
            rounded->corner[i] = wlr_scene_buffer_create(
                rounded->tree, rounded->textures[i]);
    }

    rounded->user_visible = true;
    rounded->size_ok = true;

    /* Ensure initial geometry update always runs (cache starts at 0,0
     * from memset, which could match a zero-sized object on first map) */
    rounded->last_width = -1;
    rounded->last_height = -1;

    rounded_update_geometry(rounded, config, width, height);

    return true;
}

/** Position a corner mask, disabling it when it has no area. */
static void
rounded_place_corner(struct wlr_scene_buffer *corner, int x, int y, int w, int h)
{
    if (!corner)
        return;
    bool on = w > 0 && h > 0;
    wlr_scene_node_set_enabled(&corner->node, on);
    if (!on)
        return;
    wlr_scene_node_set_position(&corner->node, x, y);
    wlr_scene_buffer_set_dest_size(corner, w, h);
}

/* Clamp per-corner radii so every corner square fits the object without
 * overlapping its neighbours: each radius is capped to half of the smaller
 * dimension, which bounds adjacent sums to the object size. */
static void
rounded_clamp_corner_radii(int radii[4], int width, int height)
{
    int half_w = width / 2;
    int half_h = height / 2;
    for (int i = 0; i < 4; i++) {
        if (radii[i] < 0)
            radii[i] = 0;
        if (radii[i] > half_w)
            radii[i] = half_w;
        if (radii[i] > half_h)
            radii[i] = half_h;
    }
}

bool
rounded_update_geometry(rounded_nodes_t *rounded,
                        const rounded_config_t *config,
                        int width, int height)
{
    int r[4];
    bool any;

    if (!rounded || !rounded->tree || !config)
        return false;

    /* Skip if geometry hasn't changed - avoids redundant damage.
     * Config changes go through rounded_update_config() which does
     * destroy+create, resetting the cache via memset. */
    if (rounded->last_width == width && rounded->last_height == height)
        return false;
    rounded->last_width = width;
    rounded->last_height = height;

    for (int i = 0; i < 4; i++)
        r[i] = config->radii[i];
    rounded_clamp_corner_radii(r, width, height);
    any = r[0] > 0 || r[1] > 0 || r[2] > 0 || r[3] > 0;

    /* An object smaller than its corner masks cannot host them; hide
     * rather than let the patches overlap. */
    rounded->size_ok = width > 0 && height > 0 && any;
    if (!rounded->size_ok) {
        wlr_scene_node_set_enabled(&rounded->tree->node, false);
        return false;
    }
    wlr_scene_node_set_enabled(&rounded->tree->node,
        rounded->user_visible);

    rounded_place_corner(rounded->corner[ROUNDED_TL], 0, 0, r[ROUNDED_TL], r[ROUNDED_TL]);
    rounded_place_corner(rounded->corner[ROUNDED_TR], width - r[ROUNDED_TR], 0,
                         r[ROUNDED_TR], r[ROUNDED_TR]);
    rounded_place_corner(rounded->corner[ROUNDED_BL], 0, height - r[ROUNDED_BL],
                         r[ROUNDED_BL], r[ROUNDED_BL]);
    rounded_place_corner(rounded->corner[ROUNDED_BR], width - r[ROUNDED_BR],
                         height - r[ROUNDED_BR], r[ROUNDED_BR], r[ROUNDED_BR]);
    return true;
}

void
rounded_update_config(rounded_nodes_t *rounded,
                      struct wlr_scene_tree *parent,
                      const rounded_config_t *config,
                      int width, int height)
{
    if (!rounded || !config)
        return;

    /* Destroy existing masks and recreate with new config.
     * Corner textures are tiny so recreation is cheap. */
    rounded_destroy(rounded);

    if (config->enabled)
        rounded_create(parent, rounded, config, width, height);
}

void
rounded_set_visible(rounded_nodes_t *rounded, bool visible)
{
    if (!rounded)
        return;

    rounded->user_visible = visible;
    if (rounded->tree)
        wlr_scene_node_set_enabled(&rounded->tree->node,
            visible && rounded->size_ok);
}

void
rounded_destroy(rounded_nodes_t *rounded)
{
    if (!rounded)
        return;

    if (rounded->tree)
        wlr_scene_node_destroy(&rounded->tree->node);

    rounded_release(rounded);
}

void
rounded_release(rounded_nodes_t *rounded)
{
    if (!rounded)
        return;

    rounded_free_textures(rounded);
    memset(rounded, 0, sizeof(*rounded));
}

/* ========== True Crop ========== */

/** Scale one premultiplied ARGB8888 pixel by `keep` (0..1). */
static inline uint32_t
rounded_scale_pixel(uint32_t px, float keep)
{
    if (keep >= 1.0f)
        return px;
    if (keep <= 0.0f)
        return 0;
    uint32_t a = (px >> 24) & 0xff;
    uint32_t r = (px >> 16) & 0xff;
    uint32_t g = (px >> 8) & 0xff;
    uint32_t b = px & 0xff;
    a = (uint32_t)(a * keep + 0.5f);
    r = (uint32_t)(r * keep + 0.5f);
    g = (uint32_t)(g * keep + 0.5f);
    b = (uint32_t)(b * keep + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void
rounded_crop_pixels(void *pixels, size_t stride, int buf_w, int buf_h,
                    double origin_x, double origin_y,
                    double scale_x, double scale_y,
                    const struct wlr_box *rrect, const int radii[4])
{
    int r[4];
    bool any = false;

    if (!pixels || !rrect || !radii || buf_w <= 0 || buf_h <= 0
            || scale_x <= 0.0 || scale_y <= 0.0)
        return;

    for (int i = 0; i < 4; i++)
        r[i] = radii[i];
    rounded_clamp_corner_radii(r, rrect->width, rrect->height);
    for (int i = 0; i < 4; i++)
        any = any || r[i] > 0;
    if (!any)
        return;

    /* Arc centers and the local-space square each corner occupies. */
    const double cx[4] = {
        rrect->x + r[ROUNDED_TL], rrect->x + rrect->width - r[ROUNDED_TR],
        rrect->x + r[ROUNDED_TL], rrect->x + rrect->width - r[ROUNDED_BR],
    };
    const double cy[4] = {
        rrect->y + r[ROUNDED_TL], rrect->y + r[ROUNDED_TR],
        rrect->y + rrect->height - r[ROUNDED_BL],
        rrect->y + rrect->height - r[ROUNDED_BR],
    };
    const double sx0[4] = { rrect->x, cx[1], rrect->x, cx[3] };
    const double sy0[4] = { rrect->y, rrect->y, cy[2], cy[3] };

    uint8_t *base = pixels;
    for (int i = 0; i < 4; i++) {
        if (r[i] <= 0)
            continue;
        /* Map the corner square to a buffer pixel range (inclusive of
         * partially covered edge pixels), clipped to the buffer. */
        int px0 = (int)floor((sx0[i] - origin_x) * scale_x);
        int py0 = (int)floor((sy0[i] - origin_y) * scale_y);
        int px1 = (int)ceil((sx0[i] + r[i] - origin_x) * scale_x);
        int py1 = (int)ceil((sy0[i] + r[i] - origin_y) * scale_y);
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 > buf_w) px1 = buf_w;
        if (py1 > buf_h) py1 = buf_h;
        if (px0 >= px1 || py0 >= py1)
            continue;

        for (int py = py0; py < py1; py++) {
            uint32_t *row = (uint32_t *)(base + (size_t)py * stride);
            double ly = origin_y + (py + 0.5) / scale_y;
            double dy = ly - cy[i];
            for (int px = px0; px < px1; px++) {
                double lx = origin_x + (px + 0.5) / scale_x;
                double dx = lx - cx[i];
                /* Only the outer quadrant of the square is curved. */
                if ((i == 0 || i == 2) ? dx > 0 : dx < 0)
                    continue;
                if ((i == 0 || i == 1) ? dy > 0 : dy < 0)
                    continue;
                float d = (float)sqrt(dx * dx + dy * dy);
                float keep = 1.0f - rounded_cut_alpha(d, (float)r[i]);
                row[px] = rounded_scale_pixel(row[px], keep);
            }
        }
    }
}

struct wlr_buffer *
rounded_crop_copy_texture(struct wlr_texture *texture)
{
    if (!texture || texture->width <= 0 || texture->height <= 0)
        return NULL;

    struct wlr_buffer *wlr_buf = rounded_buffer_create(texture->width,
                                                       texture->height);
    if (!wlr_buf)
        return NULL;

    struct rounded_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
    struct wlr_texture_read_pixels_options opts = {
        .data = buffer->data,
        .format = DRM_FORMAT_ARGB8888,
        .stride = (uint32_t)buffer->stride,
    };
    if (!wlr_texture_read_pixels(texture, &opts)) {
        wlr_buffer_drop(wlr_buf);
        return NULL;
    }
    return wlr_buf;
}

/**
 * Copy raw CPU pixels into a new ARGB8888 buffer (the crop copies from
 * this). Unlike the texture path this needs no renderer-specific readback,
 * so it works on every backend (shm clients under vulkan/gles2 included).
 * Only ARGB8888/XRGB8888 sources are accepted; XRGB alpha is forced opaque.
 */
struct wlr_buffer *
rounded_crop_copy_data(int width, int height, const void *data,
                       size_t stride, uint32_t format)
{
    struct wlr_buffer *wlr_buf;
    struct rounded_buffer *buffer;
    const uint8_t *src = data;
    uint8_t *dst;
    int x, y;

    if (!data || width <= 0 || height <= 0)
        return NULL;
    if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888)
        return NULL;

    wlr_buf = rounded_buffer_create(width, height);
    if (!wlr_buf)
        return NULL;
    buffer = wl_container_of(wlr_buf, buffer, base);
    dst = buffer->data;

    for (y = 0; y < height; y++) {
        const uint8_t *s = src + (size_t)y * stride;
        uint8_t *d = dst + (size_t)y * buffer->stride;
        if (format == DRM_FORMAT_XRGB8888) {
            for (x = 0; x < width; x++) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
                d[x * 4 + 3] = 0xff;
            }
        } else {
            memcpy(d, s, (size_t)width * 4);
        }
    }
    return wlr_buf;
}

/** Append a rounded rectangle path with per-corner radii (0 = right angle).
 * Corners ordered TL, TR, BL, BR; arcs centered at each corner square's
 * inner corner (mirrors gears.shape.rounded_rect). */
static void
rounded_cairo_path_radii(cairo_t *cr, double x, double y, double w, double h,
                         const double r[4])
{
    static const double pi = 3.14159265358979323846;
    double r0 = r[ROUNDED_TL] > 0.0 ? r[ROUNDED_TL] : 0.0;
    double r1 = r[ROUNDED_TR] > 0.0 ? r[ROUNDED_TR] : 0.0;
    double r2 = r[ROUNDED_BL] > 0.0 ? r[ROUNDED_BL] : 0.0;
    double r3 = r[ROUNDED_BR] > 0.0 ? r[ROUNDED_BR] : 0.0;

    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + r0, y);
    cairo_line_to(cr, x + w - r1, y);
    cairo_arc(cr, x + w - r1, y + r1, r1, -pi / 2, 0);
    cairo_arc(cr, x + w - r3, y + h - r3, r3, 0, pi / 2);
    cairo_line_to(cr, x + r2, y + h);
    cairo_arc(cr, x + r2, y + h - r2, r2, pi / 2, pi);
    cairo_arc(cr, x + r0, y + r0, r0, pi, 3 * pi / 2);
    cairo_close_path(cr);
}

struct wlr_buffer *
rounded_crop_render_ring(int frame_w, int frame_h, int bw, const int radii[4],
                         const float color[4])
{
    double outer[4], inner[4];
    int inner_w, inner_h;

    if (frame_w <= 0 || frame_h <= 0 || bw <= 0 || !color || !radii)
        return NULL;

    for (int i = 0; i < 4; i++)
        outer[i] = radii[i] < 0 ? 0 : (double)radii[i];
    /* Clamp each outer radius to the frame (avoids overlapping corners). */
    {
        int r[4];
        for (int i = 0; i < 4; i++)
            r[i] = (int)outer[i];
        rounded_clamp_corner_radii(r, frame_w, frame_h);
        for (int i = 0; i < 4; i++)
            outer[i] = r[i];
    }

    inner_w = frame_w - 2 * bw;
    inner_h = frame_h - 2 * bw;
    for (int i = 0; i < 4; i++) {
        inner[i] = outer[i] - bw;
        if (inner[i] < 0)
            inner[i] = 0;
    }
    {
        int r[4];
        for (int i = 0; i < 4; i++)
            r[i] = (int)inner[i];
        rounded_clamp_corner_radii(r, inner_w, inner_h);
        for (int i = 0; i < 4; i++)
            inner[i] = r[i];
    }

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, frame_w, frame_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    rounded_cairo_path_radii(cr, 0, 0, frame_w, frame_h, outer);
    if (inner_w > 0 && inner_h > 0) {
        rounded_cairo_path_radii(cr, bw, bw, inner_w, inner_h, inner);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    struct wlr_buffer *wlr_buf = rounded_buffer_create(frame_w, frame_h);
    if (wlr_buf) {
        struct rounded_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
        const unsigned char *src = cairo_image_surface_get_data(surface);
        int src_stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < frame_h; y++)
            memcpy((uint8_t *)buffer->data + (size_t)y * buffer->stride,
                   src + (size_t)y * src_stride, (size_t)frame_w * 4);
    }
    cairo_surface_destroy(surface);
    return wlr_buf;
}

struct wlr_buffer *
rounded_crop_render_innerline(int content_w, int content_h, int bw,
                              const int radii[4], double line_w,
                              const float color[4])
{
    double inner[4], center[4];
    double cx, cy, cw, ch;

    if (content_w <= 0 || content_h <= 0 || line_w <= 0
            || !radii || !color)
        return NULL;
    if (line_w >= content_w || line_w >= content_h)
        return NULL;

    /* The ring's inner contour: radii inset by the border width, clamped the
     * same way rounded_crop_render_ring() clamps them. This is the contour
     * the hairline must hug so both share the exact same arcs. */
    for (int i = 0; i < 4; i++) {
        double r = radii[i] < 0 ? 0.0 : (double)radii[i];
        r = r - bw;
        if (r < 0.0)
            r = 0.0;
        inner[i] = r;
    }
    {
        int r[4];
        for (int i = 0; i < 4; i++)
            r[i] = (int)inner[i];
        rounded_clamp_corner_radii(r, content_w, content_h);
        for (int i = 0; i < 4; i++)
            inner[i] = r[i];
    }

    /* Centerline radius: the stroke's outer edge then lies exactly on the
     * ring's inner contour (outer-edge radius = centerline + line_w/2). */
    for (int i = 0; i < 4; i++) {
        center[i] = inner[i] - line_w / 2.0;
        if (center[i] < 0.0)
            center[i] = 0.0;
    }

    cx = line_w / 2.0;
    cy = line_w / 2.0;
    cw = content_w - line_w;
    ch = content_h - line_w;

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, content_w, content_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, line_w);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    rounded_cairo_path_radii(cr, cx, cy, cw, ch, center);
    cairo_stroke(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    struct wlr_buffer *wlr_buf = rounded_buffer_create(content_w, content_h);
    if (wlr_buf) {
        struct rounded_buffer *buffer = wl_container_of(wlr_buf, buffer, base);
        const unsigned char *src = cairo_image_surface_get_data(surface);
        int src_stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < content_h; y++)
            memcpy((uint8_t *)buffer->data + (size_t)y * buffer->stride,
                   src + (size_t)y * src_stride, (size_t)content_w * 4);
    }
    cairo_surface_destroy(surface);
    return wlr_buf;
}

void
rounded_crop_release(rounded_crop_t *crop)
{
    if (!crop)
        return;
    if (crop->buf)
        wlr_buffer_drop(crop->buf);
    if (crop->ring_buf)
        wlr_buffer_drop(crop->ring_buf);
    if (crop->innerline_buf)
        wlr_buffer_drop(crop->innerline_buf);
    memset(crop, 0, sizeof(*crop));
}

/* ========== Lua Integration ========== */

/** Per-corner Lua keys, ordered TL, TR, BL, BR. */
static const char *const rounded_corner_keys[4] = {
    "top_left", "top_right", "bottom_left", "bottom_right"
};

/**
 * Parse a radius value ({@code number} or table) into per-corner radii.
 * A plain number sets all four corners; a positional table {tl,tr,bl,br}
 * and/or named keys set individual corners.
 */
static void
rounded_parse_radii_value(lua_State *L, int idx, int radii[4])
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
            rounded_parse_radii_value(L, -1, radii);
        lua_pop(L, 1);
        lua_getfield(L, idx, "corner_radii");
        if (!lua_isnil(L, -1))
            rounded_parse_radii_value(L, -1, radii);
        lua_pop(L, 1);
        /* Named per-corner keys at this level */
        for (int i = 0; i < 4; i++) {
            lua_getfield(L, idx, rounded_corner_keys[i]);
            if (lua_isnumber(L, -1))
                radii[i] = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    }
}

bool
rounded_config_from_lua(lua_State *L, int idx, rounded_config_t *config,
                        bool is_drawin)
{
    if (!config)
        return false;

    /* Start from theme defaults (not hardcoded defaults) */
    *config = *rounded_get_effective_config(NULL, is_drawin);

    if (lua_isboolean(L, idx)) {
        config->enabled = lua_toboolean(L, idx);
        return true;
    }

    /* A plain number sets an uniform radius (0 = disabled) */
    if (lua_isnumber(L, idx)) {
        int n = (int)lua_tointeger(L, idx);
        rounded_config_set_uniform(config, n);
        config->enabled = n > 0;
        return true;
    }

    if (lua_isnil(L, idx)) {
        config->enabled = false;
        return true;
    }

    if (!lua_istable(L, idx)) {
        lua_pushstring(L,
            "corner_radius must be boolean, number or table");
        return false;
    }

    /* Parse table fields */
    config->enabled = true;

    lua_getfield(L, idx, "enabled");
    if (!lua_isnil(L, -1))
        config->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    /* `radius` = uniform number or per-corner table */
    lua_getfield(L, idx, "radius");
    if (!lua_isnil(L, -1))
        rounded_parse_radii_value(L, -1, config->radii);
    lua_pop(L, 1);

    /* `corner_radii` = positional {tl, tr, bl, br} */
    lua_getfield(L, idx, "corner_radii");
    if (!lua_isnil(L, -1))
        rounded_parse_radii_value(L, -1, config->radii);
    lua_pop(L, 1);

    /* Named per-corner keys at table level */
    for (int i = 0; i < 4; i++) {
        lua_getfield(L, idx, rounded_corner_keys[i]);
        if (lua_isnumber(L, -1))
            config->radii[i] = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

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

    config->radius = config->radii[ROUNDED_TL];

    return true;
}

void
rounded_config_to_lua(lua_State *L, const rounded_config_t *config)
{
    bool uniform;
    int i;

    if (!config) {
        lua_pushnil(L);
        return;
    }

    if (!config->enabled) {
        lua_pushboolean(L, false);
        return;
    }

    uniform = true;
    for (i = 1; i < 4; i++)
        uniform = uniform && config->radii[i] == config->radii[0];

    lua_newtable(L);

    lua_pushboolean(L, config->enabled);
    lua_setfield(L, -2, "enabled");

    if (uniform) {
        lua_pushinteger(L, config->radii[ROUNDED_TL]);
        lua_setfield(L, -2, "radius");
    } else {
        lua_createtable(L, 4, 0);
        for (i = 0; i < 4; i++) {
            lua_pushinteger(L, config->radii[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "corner_radii");
    }

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

/** Read one beautiful radius key (number or {tl,tr,bl,br}) into radii. */
static void
rounded_beautiful_radius(lua_State *L, const char *key, int radii[4])
{
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1))
        rounded_parse_radii_value(L, -1, radii);
    lua_pop(L, 1);
}

/** Read one color beautiful key into a float[4] if set and valid. */
static void
rounded_beautiful_color(lua_State *L, const char *key, float out[4])
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
rounded_load_beautiful_defaults(lua_State *L)
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
    globalconf.rounded.client = rounded_defaults;
    globalconf.rounded.drawin = rounded_defaults;

    rounded_config_t *client = &globalconf.rounded.client;

    /* Client rounded corner defaults */
    lua_getfield(L, -1, "corner_enabled");
    if (!lua_isnil(L, -1))
        client->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    rounded_beautiful_radius(L, "corner_radius", client->radii);
    client->radius = client->radii[ROUNDED_TL];
    rounded_beautiful_color(L, "corner_color", client->color);

    /* Copy client defaults to drawin, then apply drawin-specific overrides */
    globalconf.rounded.drawin = *client;
    rounded_config_t *drawin = &globalconf.rounded.drawin;

    lua_getfield(L, -1, "corner_drawin_enabled");
    if (!lua_isnil(L, -1))
        drawin->enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    rounded_beautiful_radius(L, "corner_drawin_radius", drawin->radii);
    drawin->radius = drawin->radii[ROUNDED_TL];
    rounded_beautiful_color(L, "corner_drawin_color", drawin->color);

    lua_pop(L, 1);  /* Pop beautiful table */
}

/* vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80 */