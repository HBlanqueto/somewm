/* window.c - Rounded crop + inner hairline helpers (ported to release/1.4)
 *
 * On upstream main these helpers live in a full window-lifecycle module; on
 * release/1.4 the lifecycle stays in somewm.c and this file only carries the
 * feature-specific surface: rounded-corner crop (content, titlebars, child
 * subsurfaces), the rounded border ring, and the macOS-style inner hairline.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "somewm_types.h"
#include "window.h"
#include "event_queue.h"
#include "globalconf.h"
#include "objects/client.h"
#include "objects/window.h"
#include "color.h"
#include "rounded.h"

#include "luaa.h"
#include "common/luaobject.h"
#include "common/util.h"

/* Defined in somewm.c (non-static); forward-declared here so the crop config
 * change path can re-apply geometry without pulling in the lifecycle module. */
void apply_geometry_to_wlroots(Client *c);

/* LISTEN is defined in somewm.c; mirror it here for the child-surface hooks
 * this module owns. */
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))

/* Minimal client accessors. The top-level client.h also carries inline
 * helpers that depend on somewm.c globals (seat), so only the two surface
 * accessors this module needs are reproduced here. */
static inline bool
window_client_has_surface(Client *c)
{
#ifdef XWAYLAND
	if (c->client_type == X11)
		return c->surface.xwayland != NULL;
#endif
	return c->surface.xdg != NULL;
}

static inline struct wlr_surface *
window_client_surface(Client *c)
{
#ifdef XWAYLAND
	if (c->client_type == X11)
		return c->surface.xwayland->surface;
#endif
	return c->surface.xdg->surface;
}

/* ========== Inner hairline theme accessors ========== */

unsigned int
get_border_inner_width(void)
{
	lua_State *L = globalconf_get_lua_State();
	if (!L) return globalconf.appearance.border_inner_width;

	lua_getglobal(L, "require");
	lua_pushstring(L, "beautiful");
	if (lua_pcall(L, 1, 1, 0) != 0) {
		lua_pop(L, 1);
		return globalconf.appearance.border_inner_width;
	}
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "border_inner_width");
		if (lua_isnumber(L, -1)) {
			unsigned int val = lua_tointeger(L, -1);
			lua_pop(L, 2);
			return val;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	return globalconf.appearance.border_inner_width;
}

const float *
get_border_inner_color(void)
{
	static float parsed[4];
	lua_State *L = globalconf_get_lua_State();

	if (L) {
		lua_getglobal(L, "require");
		lua_pushstring(L, "beautiful");
		if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
			lua_getfield(L, -1, "border_inner_color");
			if (lua_isstring(L, -1)) {
				color_t c;
				if (color_init_from_string(&c, lua_tostring(L, -1))) {
					parsed[0] = c.red / 255.0f;
					parsed[1] = c.green / 255.0f;
					parsed[2] = c.blue / 255.0f;
					parsed[3] = c.alpha / 255.0f;
					lua_pop(L, 1);
					lua_pop(L, 1);
					return parsed;
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}

	return globalconf.appearance.border_inner_color;
}

bool
get_border_inner_enabled(void)
{
	lua_State *L = globalconf_get_lua_State();
	if (!L) return globalconf.appearance.border_inner_enabled;

	lua_getglobal(L, "require");
	lua_pushstring(L, "beautiful");
	if (lua_pcall(L, 1, 1, 0) != 0) {
		lua_pop(L, 1);
		return globalconf.appearance.border_inner_enabled;
	}
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "border_inner_enabled");
		if (!lua_isnil(L, -1)) {
			bool val = lua_toboolean(L, -1);
			lua_pop(L, 2);
			return val;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	return globalconf.appearance.border_inner_enabled;
}

bool
get_border_inner_drawin_enabled(void)
{
	lua_State *L = globalconf_get_lua_State();
	if (!L) return globalconf.appearance.border_inner_drawin_enabled;

	lua_getglobal(L, "require");
	lua_pushstring(L, "beautiful");
	if (lua_pcall(L, 1, 1, 0) != 0) {
		lua_pop(L, 1);
		return globalconf.appearance.border_inner_drawin_enabled;
	}
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "border_inner_drawin_enabled");
		if (!lua_isnil(L, -1)) {
			bool val = lua_toboolean(L, -1);
			lua_pop(L, 2);
			return val;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	return globalconf.appearance.border_inner_drawin_enabled;
}

void
client_init_border_inner_defaults(Client *c)
{
	const float *bi = get_border_inner_color();
	c->border_inner_color.red = (uint8_t)((bi[0] * 255.0f) + 0.5f);
	c->border_inner_color.green = (uint8_t)((bi[1] * 255.0f) + 0.5f);
	c->border_inner_color.blue = (uint8_t)((bi[2] * 255.0f) + 0.5f);
	c->border_inner_color.alpha = (uint8_t)((bi[3] * 255.0f) + 0.5f);
	c->border_inner_color.initialized = true;
	c->border_inner_width = get_border_inner_width();
	c->border_inner_enabled = get_border_inner_enabled();
}

/* ========== Rounded corner crop (true transparency) ==========
 *
 * Clients round their corners by cropping, not by overlaying masks: the
 * committed toplevel buffer is copied and its corner pixels are faded to
 * transparent, titlebar buffers get the same treatment, and the square
 * border rects are replaced by a rounded ring. The frame's outer edge is
 * a rounded rect of the configured radius; content and titlebars are
 * clipped by the concentric inner rounded rect (inset by the border).
 */

/* Effective per-corner radii of the outer frame contour. Returns false when
 * rounding is off (disabled, fullscreen, or every corner radius is 0). */
static bool
client_crop_radii(Client *c, int radii[4])
{
	const rounded_config_t *cfg =
		rounded_get_effective_config(c->rounded_config, false);
	int i, any = 0;

	for (i = 0; i < 4; i++)
		radii[i] = cfg ? cfg->radii[i] : 0;
	if (!cfg || !cfg->enabled || c->fullscreen) {
		for (i = 0; i < 4; i++)
			radii[i] = 0;
		return false;
	}
	for (i = 0; i < 4; i++)
		any += radii[i] > 0;
	return any > 0;
}

/* Effective per-corner radii of the outer frame contour. Returns false when
 * rounding is off (disabled, fullscreen, or every corner radius is 0). */
bool
client_crop_active(Client *c)
{
	int radii[4];
	return client_crop_radii(c, radii);
}

bool
client_crop_outer_radii(Client *c, int radii[4])
{
	return client_crop_radii(c, radii);
}

/* Inner rounded rect (frame coordinates) shared by content and titlebars.
 * Computes the per-corner radii of the inner rect (outer radius minus the
 * border, clamped to the content area). Returns false when nothing should
 * be cropped. */
bool
client_crop_inner_rect(Client *c, struct wlr_box *rect, int radii[4])
{
	int frame_radii[4];
	int i;

	*rect = (struct wlr_box){
		.x = c->bw,
		.y = c->bw,
		.width = c->geometry.width,
		.height = c->geometry.height,
	};
	if (!client_crop_radii(c, frame_radii))
		return false;

	for (i = 0; i < 4; i++) {
		radii[i] = frame_radii[i] - c->bw;
		if (radii[i] < 0)
			radii[i] = 0;
		if (radii[i] * 2 > rect->width)
			radii[i] = rect->width / 2;
		if (radii[i] * 2 > rect->height)
			radii[i] = rect->height / 2;
		if (radii[i] < 0)
			radii[i] = 0;
	}
	return radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0;
}

struct client_crop_find {
	struct wlr_surface *surface;
	struct wlr_scene_buffer *found;
};

static void
client_crop_find_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	struct client_crop_find *f = data;
	struct wlr_scene_surface *ss;

	if (f->found)
		return;
	ss = wlr_scene_surface_try_from_buffer(buffer);
	if (ss && ss->surface == f->surface)
		f->found = buffer;
}

static void
client_crop_child_commit(struct wl_listener *listener, void *data)
{
	struct client_crop_child_entry *e =
		wl_container_of(listener, e, commit);
	client_crop_apply(e->c);
}

/* A child (sub)surface is being destroyed outside the unmap path (e.g. a
 * toolbox/tooltip subsurface removed while the window stays mapped, or the
 * client shutting down). wlroots emits surface::destroy before asserting
 * that every commit/destroy listener is gone: detach ours here or the
 * compositor aborts on wl_surface destroy. */
static void
client_crop_child_destroy(struct wl_listener *listener, void *data)
{
	struct client_crop_child_entry *e =
		wl_container_of(listener, e, destroy);
	(void)data;
	if (e->hooked) {
		wl_list_remove(&e->commit.link);
		e->hooked = false;
	}
	if (e->hooked_destroy) {
		wl_list_remove(&e->destroy.link);
		e->hooked_destroy = false;
	}
	if (e->copy) {
		wlr_buffer_drop(e->copy);
		e->copy = NULL;
	}
	e->sb = NULL;
	e->surface = NULL;
	e->raw = NULL;
	e->active = false;
}

static void
client_crop_reset_children(Client *c)
{
	for (int i = 0; i < c->n_crop_children; i++) {
		struct client_crop_child_entry *e = &c->crop_children[i];
		if (e->hooked) {
			wl_list_remove(&e->commit.link);
			e->hooked = false;
		}
		if (e->hooked_destroy) {
			wl_list_remove(&e->destroy.link);
			e->hooked_destroy = false;
		}
		if (e->copy) {
			wlr_buffer_drop(e->copy);
			e->copy = NULL;
		}
		e->sb = NULL;
		e->surface = NULL;
		e->raw = NULL;
		e->active = false;
	}
	c->n_crop_children = 0;
}

/* Restore the raw client buffer on every cropped child scene buffer. */
static void
client_crop_restore_children(Client *c)
{
	for (int i = 0; i < c->n_crop_children; i++) {
		struct client_crop_child_entry *e = &c->crop_children[i];
		if (e->active && e->sb && e->raw && e->sb->buffer == e->copy)
			wlr_scene_buffer_set_buffer(e->sb, e->raw);
		e->active = false;
	}
}

/* Where a child's rounded arc falls changes whenever the client's rounded
 * rect, scale or the child's viewport changes. Any map change invalidates
 * that copy's pixels: the readback below crops from the raw buffer, so
 * only the arc placement (stored origin/scale) can get stale. */
static void
client_crop_child_punch_opaque(struct wlr_scene_buffer *sb,
				const struct wlr_box *local, const int r[4],
				double bx, double by)
{
	/* The scene buffer's opaque region lives in the buffer node's own
	 * local space, which is offset by (bx,by) from the rounded rect. */
	struct wlr_box rel = {
		.x = local->x - bx, .y = local->y - by,
		.width = local->width, .height = local->height,
	};
	pixman_region32_t opaque, holes;
	pixman_box32_t boxes[4] = {
		{ rel.x, rel.y, rel.x + r[0], rel.y + r[0] },
		{ rel.x + rel.width - r[1], rel.y,
		  rel.x + rel.width, rel.y + r[1] },
		{ rel.x, rel.y + rel.height - r[2],
		  rel.x + r[2], rel.y + rel.height },
		{ rel.x + rel.width - r[3], rel.y + rel.height - r[3],
		  rel.x + rel.width, rel.y + rel.height },
	};
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &sb->opaque_region);
	pixman_region32_init_rects(&holes, boxes, 4);
	pixman_region32_subtract(&opaque, &opaque, &holes);
	wlr_scene_buffer_set_opaque_region(sb, &opaque);
	pixman_region32_fini(&holes);
	pixman_region32_fini(&opaque);
}

/* Re-crop one child (sub)surface buffer. `sx`,`sy` is the scene buffer's
 * position reported by for_each_buffer (relative to the starting node's
 * parent, i.e. including scene_surface's own position); the rounded rect
 * lives in scene_surface node-local space, so (bx,by) = (sx,sy) minus the
 * scene_surface node position. */
static void
client_crop_child_apply(Client *c, struct wlr_scene_buffer *sb,
			int sx, int sy, const struct wlr_box *local,
			const int inner_r[4])
{
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	struct wlr_surface *surface;
	struct client_crop_child_entry *e = NULL;
	double m_ox, m_oy, m_sx, m_sy;
	double bx = sx - c->scene_surface->node.x;
	double by = sy - c->scene_surface->node.y;
	bool map_changed = false, raw_changed = false;
	int i;

	if (!ss || !ss->surface || !ss->surface->buffer)
		return;
	surface = ss->surface;

	if (sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		double s = surface->current.scale > 0 ? surface->current.scale : 1.0;
		m_sx = m_sy = s;
		m_ox = bx;
		m_oy = by;
	} else {
		struct wlr_fbox src = sb->src_box;
		double dest_w = sb->dst_width;
		double dest_h = sb->dst_height;

		if (wlr_fbox_empty(&src)) {
			src.x = 0;
			src.y = 0;
			src.width = sb->buffer->width;
			src.height = sb->buffer->height;
		}
		if (dest_w <= 0) dest_w = src.width;
		if (dest_h <= 0) dest_h = src.height;
		m_sx = src.width / dest_w;
		m_sy = src.height / dest_h;
		m_ox = bx - src.x / m_sx;
		m_oy = by - src.y / m_sy;
	}

	for (i = 0; i < c->n_crop_children; i++) {
		if (c->crop_children[i].sb == sb) {
			e = &c->crop_children[i];
			break;
		}
	}
	if (e && e->surface != surface) {
		/* Rare: a new wl_surface behind the same scene buffer. Detach the
		 * old surface's hooks before re-hooking below. */
		if (e->hooked) {
			wl_list_remove(&e->commit.link);
			e->hooked = false;
		}
		if (e->hooked_destroy) {
			wl_list_remove(&e->destroy.link);
			e->hooked_destroy = false;
		}
	}
	if (!e && c->n_crop_children < 8) {
		e = &c->crop_children[c->n_crop_children++];
		memset(e, 0, sizeof(*e));
		e->c = c;
		e->sb = sb;
		e->surface = surface;
	}

	if (e) {
		map_changed = e->copy != NULL
			&& (e->origin_x != m_ox || e->origin_y != m_oy
				|| e->scale_x != m_sx || e->scale_y != m_sy);
		raw_changed = e->copy == NULL || e->raw != &surface->buffer->base;
	}

	if (e && e->copy && !c->crop.dirty && !map_changed && !raw_changed) {
		/* Reuse: same buffer + same arc placement. */
		goto show;
	}

	if (e) {
		struct wlr_buffer *buf = NULL;
		void *data = NULL;
		uint32_t fmt = 0;
		size_t stride = 0;

		if (wlr_buffer_begin_data_ptr_access(&surface->buffer->base,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
			buf = rounded_crop_copy_data(surface->buffer->base.width,
				surface->buffer->base.height, data, stride, fmt);
			wlr_buffer_end_data_ptr_access(&surface->buffer->base);
		}
		if (!buf) {
			struct wlr_texture *tex = wlr_surface_get_texture(surface);
			buf = tex ? rounded_crop_copy_texture(tex) : NULL;
		}
		if (!buf)
			return;
		if (wlr_buffer_begin_data_ptr_access(buf,
				WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride)) {
			rounded_crop_pixels(data, stride, buf->width, buf->height,
				m_ox, m_oy, m_sx, m_sy, local, inner_r);
			wlr_buffer_end_data_ptr_access(buf);
		}
		if (e->copy)
			wlr_buffer_drop(e->copy);
		e->copy = buf;
		e->raw = &surface->buffer->base;
		e->origin_x = m_ox;
		e->origin_y = m_oy;
		e->scale_x = m_sx;
		e->scale_y = m_sy;
		e->surface = surface;
		if (!e->hooked) {
			LISTEN(&surface->events.commit, &e->commit, client_crop_child_commit);
			e->hooked = true;
		}
		if (!e->hooked_destroy) {
			LISTEN(&surface->events.destroy, &e->destroy, client_crop_child_destroy);
			e->hooked_destroy = true;
		}
	} else {
		return;
	}

show:
	{
		struct wlr_scene_buffer_set_buffer_options opts = {
			.damage = c->crop.dirty ? NULL : &surface->buffer_damage,
		};
		wlr_scene_buffer_set_buffer_with_options(sb, e->copy, &opts);
		client_crop_child_punch_opaque(sb, local, inner_r, bx, by);
		e->active = true;
	}
}

/* Collect every scene buffer under the client tree except the toplevel's. */
struct client_crop_scan {
	Client *c;
	struct { struct wlr_scene_buffer *sb; int sx, sy; } list[8];
	int n;
};

static void
client_crop_child_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	struct client_crop_scan *scan = data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
	struct wlr_surface *top;

	if (ss && scan->n < 8) {
		top = window_client_surface(scan->c);
		if (ss->surface != top && buffer->buffer != scan->c->crop.buf) {
			scan->list[scan->n].sb = buffer;
			scan->list[scan->n].sx = sx;
			scan->list[scan->n].sy = sy;
			scan->n++;
		}
	}
}

void
client_crop_apply(Client *c)
{
	struct wlr_surface *surface;
	struct wlr_scene_buffer *sb;
	struct wlr_box inner, local;
	struct client_crop_find find;
	int inner_r[4], tl, tt;
	bool inner_active, reuse;

	if (!c->scene || !c->scene_surface || !window_client_has_surface(c))
		return;
	surface = window_client_surface(c);
	if (!surface || !surface->buffer)
		return;

	find = (struct client_crop_find){ .surface = surface };
	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
		client_crop_find_iter, &find);
	sb = find.found;
	if (!sb)
		return;

	inner_active = client_crop_inner_rect(c, &inner, inner_r);
	if (!inner_active) {
		/* Rounding off. If our copy is still on screen (config change
		 * without a new commit), put the raw client buffer back. */
		if (c->crop.applied && sb->buffer == c->crop.buf)
			wlr_scene_buffer_set_buffer(sb, &surface->buffer->base);
		client_crop_restore_children(c);
		if (c->crop.buf) {
			wlr_buffer_drop(c->crop.buf);
			c->crop.buf = NULL;
		}
		c->crop.src = NULL;
		c->crop.applied = false;
		return;
	}

	/* The content node sits at frame (bw + titlebar_left, bw + titlebar_top);
	 * express the inner rounded rect in the node's local coordinates. */
	tl = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_LEFT].size;
	tt = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_TOP].size;
	local = (struct wlr_box){
		.x = inner.x - (c->bw + tl),
		.y = inner.y - (c->bw + tt),
		.width = inner.width,
		.height = inner.height,
	};

	/* Buffer -> node-local mapping. wlroots displays the src_box region of
	 * the buffer scaled to dest_size, so buffer pixel (px, py) lands at
	 *   node-local = (px - src.x) * dest_w / src.w, ...
	 * expressed as origin + (px + 0.5) / scale. Deriving it from
	 * src_box/dest (instead of surface scale + clip) keeps the cut exact
	 * under fractional scale and viewport scaling. Rotated outputs keep the
	 * old scale/clip heuristic (approx). */
	{
		struct wlr_fbox src = sb->src_box;
		double dest_w = sb->dst_width;
		double dest_h = sb->dst_height;
		double m_ox, m_oy, m_sx, m_sy;

		if (sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
			double s = surface->current.scale > 0 ? surface->current.scale : 1.0;
			m_sx = m_sy = s;
			m_ox = m_oy = 0.0;
		} else {
			if (wlr_fbox_empty(&src)) {
				src.x = 0;
				src.y = 0;
				src.width = sb->buffer->width;
				src.height = sb->buffer->height;
			}
			if (dest_w <= 0) dest_w = src.width;
			if (dest_h <= 0) dest_h = src.height;
			m_sx = src.width / dest_w;
			m_sy = src.height / dest_h;
			m_ox = -src.x * (dest_w / src.width);
			m_oy = -src.y * (dest_h / src.height);
		}

		/* Any change to where the arcs fall invalidates every pixel of the
		 * previous copy, not just the client's damage. */
		if (c->crop.buf
				&& (memcmp(c->crop.radii, inner_r, sizeof(c->crop.radii)) != 0
					|| c->crop.scale_x != m_sx || c->crop.scale_y != m_sy
					|| c->crop.origin_x != m_ox || c->crop.origin_y != m_oy
					|| c->crop.local.x != local.x || c->crop.local.y != local.y
					|| c->crop.local.width != local.width
					|| c->crop.local.height != local.height))
			c->crop.dirty = true;

		c->crop.scale_x = m_sx;
		c->crop.scale_y = m_sy;
		c->crop.origin_x = m_ox;
		c->crop.origin_y = m_oy;
		c->crop.local = local;
		memcpy(c->crop.radii, inner_r, sizeof(c->crop.radii));
	}

	/* Reuse the cached crop when the client buffer is unchanged (wlroots
	 * re-applies the raw buffer on every commit, even frame-only ones). */
	reuse = c->crop.buf
		&& c->crop.src == surface->buffer
		&& !c->crop.dirty
		&& pixman_region32_empty(&surface->buffer_damage);

	if (!reuse) {
		struct wlr_buffer *buf = NULL;
		void *data = NULL;
		uint32_t fmt = 0;
		size_t stride = 0;

		/* Read the client's pixels without going through the GPU: the
		 * client buffer forwards to its shm source, so this works on
		 * every renderer. DMA-BUF clients (and exotic shm formats like
		 * XRGB2101010) fall back to reading their renderer texture;
		 * wlr_surface_get_texture uploads it on demand (at commit time
		 * surface->buffer->texture is still NULL). */
		if (wlr_buffer_begin_data_ptr_access(&surface->buffer->base,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
			buf = rounded_crop_copy_data(surface->buffer->base.width,
				surface->buffer->base.height, data, stride, fmt);
			wlr_buffer_end_data_ptr_access(&surface->buffer->base);
		}
		if (!buf) {
			struct wlr_texture *tex = wlr_surface_get_texture(surface);
			buf = tex ? rounded_crop_copy_texture(tex) : NULL;
		}
		if (!buf) {
			static bool warned;
			if (!warned) {
				warn("rounded corners: cannot read client buffer "
					"(shm access and texture readback both failed)");
				warned = true;
			}
			return;
		}
		if (wlr_buffer_begin_data_ptr_access(buf,
				WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride)) {
			rounded_crop_pixels(data, stride, buf->width, buf->height,
				c->crop.origin_x, c->crop.origin_y,
				c->crop.scale_x, c->crop.scale_y, &local, inner_r);
			wlr_buffer_end_data_ptr_access(buf);
		}
		if (c->crop.buf)
			wlr_buffer_drop(c->crop.buf);
		c->crop.buf = buf;
		c->crop.src = surface->buffer;
	}

	{
		/* A geometry/config change invalidates every pixel; otherwise the
		 * client's own damage is exact (non-damaged pixels are identical
		 * between the raw buffer and our copy). */
		struct wlr_scene_buffer_set_buffer_options opts = {
			.damage = c->crop.dirty ? NULL : &surface->buffer_damage,
		};
		wlr_scene_buffer_set_buffer_with_options(sb, c->crop.buf, &opts);
	}

	/* The corners are translucent now: punch them out of the opaque
	 * region so the scene renders whatever lies behind them. */
	{
		pixman_region32_t opaque, holes;
		int r0 = inner_r[0], r1 = inner_r[1];
		int r2 = inner_r[2], r3 = inner_r[3];
		pixman_box32_t boxes[4] = {
			{ local.x, local.y, local.x + r0, local.y + r0 },
			{ local.x + local.width - r1, local.y,
			  local.x + local.width, local.y + r1 },
			{ local.x, local.y + local.height - r2,
			  local.x + r2, local.y + local.height },
			{ local.x + local.width - r3, local.y + local.height - r3,
			  local.x + local.width, local.y + local.height },
		};
		pixman_region32_init(&opaque);
		pixman_region32_copy(&opaque, &sb->opaque_region);
		pixman_region32_init_rects(&holes, boxes, 4);
		pixman_region32_subtract(&opaque, &opaque, &holes);
		wlr_scene_buffer_set_opaque_region(sb, &opaque);
		pixman_region32_fini(&holes);
		pixman_region32_fini(&opaque);
	}

	/* Round every child (sub)surface buffer too: Firefox paints its whole
	 * window in a wl_subsurface, so cropping only the toplevel leaves its
	 * corners square. Each buffer is cropped with the arcs of the same
	 * node-local rounded rect, mapped through that buffer's own view. */
	{
		struct client_crop_scan scan = { .c = c };
		bool found[8] = { false };

		wlr_scene_node_for_each_buffer(&c->scene_surface->node,
			client_crop_child_iter, &scan);
		for (int i = 0; i < scan.n; i++) {
			client_crop_child_apply(c, scan.list[i].sb,
				scan.list[i].sx, scan.list[i].sy, &local, inner_r);
			for (int j = 0; j < c->n_crop_children; j++) {
				if (c->crop_children[j].sb == scan.list[i].sb) {
					found[j] = true;
					break;
				}
			}
		}
		/* Prune children whose scene buffer left the tree (destroyed
		 * subsurfaces): their commit hook is removed and copy dropped. */
		if (c->n_crop_children > scan.n) {
			for (int i = c->n_crop_children - 1; i >= 0; i--) {
				if (found[i])
					continue;
				struct client_crop_child_entry *e = &c->crop_children[i];
				if (e->hooked) {
					wl_list_remove(&e->commit.link);
					e->hooked = false;
				}
				if (e->hooked_destroy) {
					wl_list_remove(&e->destroy.link);
					e->hooked_destroy = false;
				}
				if (e->copy)
					wlr_buffer_drop(e->copy);
				if (i < c->n_crop_children - 1)
					memmove(&c->crop_children[i], &c->crop_children[i + 1],
						sizeof(*e) * (c->n_crop_children - 1 - i));
				c->n_crop_children--;
			}
		}
	}

	c->crop.applied = true;
	c->crop.dirty = false;
}

/* Crop a titlebar buffer in place before it is handed to the scene.
 * `area` is the titlebar's rect in frame coordinates. */
void
client_crop_titlebar_buffer(Client *c, struct wlr_buffer *buffer, area_t area)
{
	struct wlr_box inner, local;
	void *data;
	uint32_t fmt;
	size_t stride;
	double scale;
	int inner_r[4];

	if (!buffer || area.width <= 0 || area.height <= 0)
		return;
	if (!client_crop_inner_rect(c, &inner, inner_r))
		return;

	local = (struct wlr_box){
		.x = inner.x - area.x,
		.y = inner.y - area.y,
		.width = inner.width,
		.height = inner.height,
	};
	/* The buffer is scaled to the titlebar area by dest_size */
	scale = (double)buffer->width / area.width;

	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride))
		return;
	if (fmt == DRM_FORMAT_ARGB8888 || fmt == DRM_FORMAT_XRGB8888)
		rounded_crop_pixels(data, stride, buffer->width, buffer->height,
			0, 0, scale, scale, &local, inner_r);
	wlr_buffer_end_data_ptr_access(buffer);
}

/* The ring is pure decoration; it must never intercept clicks. A NULL
 * point_accepts_input would make wlr_scene_node_at return it as the hit
 * (swallowing all input over the frame), so reject explicitly. */
static bool
ring_point_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

void
client_crop_update_ring(Client *c, int frame_w, int frame_h)
{
	int radii[4];
	bool active, want;
	const float *color;

	active = client_crop_radii(c, radii);
	want = active && c->bw > 0;

	if (!want) {
		if (c->crop.ring) {
			wlr_scene_buffer_set_buffer(c->crop.ring, NULL);
			wlr_scene_node_set_enabled(&c->crop.ring->node, false);
		}
		if (c->crop.ring_buf) {
			wlr_buffer_drop(c->crop.ring_buf);
			c->crop.ring_buf = NULL;
		}
		c->crop.ring_w = c->crop.ring_h = 0;
		return;
	}

	/* Border rects collapse to nothing; the ring draws the border. */
	for (int i = 0; i < 4; i++)
		wlr_scene_rect_set_size(c->border[i], 0, 0);

	if (!c->crop.ring) {
		c->crop.ring = wlr_scene_buffer_create(c->scene, NULL);
		if (!c->crop.ring)
			return;
		c->crop.ring->point_accepts_input = ring_point_accepts_input;
		c->crop.ring->node.data = c->scene->node.data;
		/* Stack with the borders it replaces (above the shadow, below
		 * content, titlebars and popups). */
		wlr_scene_node_place_above(&c->crop.ring->node, &c->border[3]->node);
	}

	color = c->border[0]->color;
	if (!c->crop.ring_buf
			|| c->crop.ring_w != frame_w || c->crop.ring_h != frame_h
			|| c->crop.ring_bw != (int)c->bw
			|| memcmp(c->crop.ring_radii, radii,
				sizeof(c->crop.ring_radii)) != 0
			|| memcmp(c->crop.ring_color, color,
				sizeof(c->crop.ring_color)) != 0) {
		struct wlr_buffer *buf = rounded_crop_render_ring(frame_w, frame_h,
			c->bw, radii, color);
		if (!buf)
			return;
		if (c->crop.ring_buf)
			wlr_buffer_drop(c->crop.ring_buf);
		c->crop.ring_buf = buf;
		c->crop.ring_w = frame_w;
		c->crop.ring_h = frame_h;
		c->crop.ring_bw = c->bw;
		memcpy(c->crop.ring_radii, radii, sizeof(c->crop.ring_radii));
		memcpy(c->crop.ring_color, color, sizeof(c->crop.ring_color));
		wlr_scene_buffer_set_buffer(c->crop.ring, buf);
		wlr_scene_buffer_set_dest_size(c->crop.ring, frame_w, frame_h);
	}
	wlr_scene_node_set_position(&c->crop.ring->node, 0, 0);
	wlr_scene_node_set_enabled(&c->crop.ring->node, true);
}

/* The inner hairline is pure decoration; it must never intercept clicks
 * (reuse ring_point_accepts_input above). */
static bool
innerline_point_accepts_input(struct wlr_scene_buffer *buffer, double *sx,
                              double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* macOS-style inner hairline: a thin line drawn around the whole window,
 * hugging the union contour of content and titlebars (the visible rounded
 * shape formed by the crop). Pure decoration - it changes no geometry and
 * captures no input. Its on/off, width and color come from the theme
 * (beautiful.border_inner_*) or the per-client override (c->border_inner_*);
 * the alpha is used as-is, no focus-based dimming. Re-renders automatically:
 * values are checked against the render cache each geometry pass, so property
 * updates and focus-independent changes are picked up on the next refresh. */
void
client_crop_update_innerline(Client *c)
{
	int radii[4], cw, ch, bw;
	float eff[4];
	double lw;

	if (!c->scene || !c->scene_surface || !c->border[0])
		return;

	lw = c->border_inner_width;
	cw = c->geometry.width;
	ch = c->geometry.height;
	bw = c->bw;

	if (!c->border_inner_enabled || lw <= 0.0 || cw <= 0 || ch <= 0
			|| c->fullscreen) {
		if (c->crop.innerline)
			wlr_scene_node_set_enabled(&c->crop.innerline->node, false);
		return;
	}

	if (c->border_inner_color.initialized)
		color_to_floats(&c->border_inner_color, eff);
	else {
		const float *base = get_border_inner_color();
		for (int i = 0; i < 4; i++)
			eff[i] = base[i];
	}

	client_crop_radii(c, radii);

	if (!c->crop.innerline) {
		c->crop.innerline = wlr_scene_buffer_create(c->scene, NULL);
		if (!c->crop.innerline)
			return;
		c->crop.innerline->node.data = c->scene->node.data;
		c->crop.innerline->point_accepts_input = innerline_point_accepts_input;
		wlr_scene_buffer_set_filter_mode(c->crop.innerline,
			WLR_SCALE_FILTER_BILINEAR);
	}

	if (!c->crop.innerline_buf
			|| c->crop.innerline_w != cw || c->crop.innerline_h != ch
			|| c->crop.innerline_bw != bw
			|| c->crop.innerline_line != lw
			|| memcmp(c->crop.innerline_radii, radii,
				sizeof(c->crop.innerline_radii)) != 0
			|| memcmp(c->crop.innerline_color, eff,
				sizeof(c->crop.innerline_color)) != 0) {
		struct wlr_buffer *buf = rounded_crop_render_innerline(cw, ch, bw,
			radii, lw, eff);
		if (!buf) {
			if (c->crop.innerline)
				wlr_scene_node_set_enabled(&c->crop.innerline->node, false);
			return;
		}
		if (c->crop.innerline_buf)
			wlr_buffer_drop(c->crop.innerline_buf);
		c->crop.innerline_buf = buf;
		c->crop.innerline_w = cw;
		c->crop.innerline_h = ch;
		c->crop.innerline_bw = bw;
		c->crop.innerline_line = lw;
		memcpy(c->crop.innerline_radii, radii,
			sizeof(c->crop.innerline_radii));
		memcpy(c->crop.innerline_color, eff, sizeof(c->crop.innerline_color));
		wlr_scene_buffer_set_buffer(c->crop.innerline, buf);
		wlr_scene_buffer_set_dest_size(c->crop.innerline, cw, ch);
	}
	/* Buffer covers the whole client geometry (the union of content and
	 * titlebars); placed at (bw, bw) its single contour traces the visible
	 * window shape, seamlessly crossing any titlebar seams. */
	wlr_scene_node_set_position(&c->crop.innerline->node, bw, bw);
	wlr_scene_node_raise_to_top(&c->crop.innerline->node);
	if (c->rounded.tree)
		wlr_scene_node_raise_to_top(&c->rounded.tree->node);
	if (c->popups)
		wlr_scene_node_raise_to_top(&c->popups->node);
	wlr_scene_node_set_enabled(&c->crop.innerline->node, true);
}

/* Rounding configuration changed (per-client property or theme reload):
 * refresh ring, titlebars and content. */
void
client_crop_config_changed(Client *c)
{
	if (!c->scene)
		return;
	c->crop.dirty = true;
	apply_geometry_to_wlroots(c);
	/* Titlebars are re-cropped as they are re-rendered */
	client_refresh_partial(c, 0, 0,
		c->geometry.width + 2 * c->bw, c->geometry.height + 2 * c->bw);
}

/* Cached crop buffers are dropped here; the ring node dies with c->scene. */
void
client_crop_release(Client *c)
{
	client_crop_reset_children(c);
	rounded_crop_release(&c->crop);
}

void
cropcommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, crop_commit);
	client_crop_apply(c);
}

/* Content-change hook for Lua autocolor: one extra listener here matches
 * the crop_commit lifetime (register on map, remove on unmap) for both
 * XDG and XWayland clients. */
void
contentcommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, content_commit);

	if (!globalconf_L || !c->scene)
		return;

	luaA_object_push(globalconf_L, c);
	some_event_queue_signal0(globalconf_L, -1, SIG_SURFACE_COMMIT);
	lua_pop(globalconf_L, 1);
}