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
#include "scenefx_compat.h"
#include <wlr/util/log.h>

#include "somewm_types.h"
#include "window.h"
#include "event_queue.h"
#include "globalconf.h"
#include "objects/client.h"
#include "objects/window.h"
#include "color.h"
#include "rounded.h"
#include "macos_frame.h"

#include "luaa.h"
#include "common/luaobject.h"
#include "common/util.h"

/* Defined in somewm.c (non-static); forward-declared here so the crop config
 * change path can re-apply geometry without pulling in the lifecycle module. */
void apply_geometry_to_wlroots(Client *c);

/* Deferred crop re-apply idle machinery (see the block above
 * cropcommitnotify): scene mutations for the rounded-corner punch must never
 * run inside a wl_surface.commit emission, so they are queued as an idle
 * callback on the display loop and applied after the dispatch completes. */
extern struct wl_event_loop *event_loop;
extern struct wl_list mons;

static struct wl_event_source *crop_reapply_source;
static bool crop_reapply_pending;
static void crop_reapply(void *data);
static void crop_schedule(void);

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
	static bool deprecated_warned;

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
			if (!deprecated_warned) {
				deprecated_warned = true;
				warn("beautiful.border_inner_* is deprecated: the native "
					"macOS frame owns the inner contour now; ignored");
			}
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

	/* The built-in macOS frame clips content and titlebars to its own radius
	 * (per-client corner_radius override, else the 10 px Big Sur constant),
	 * so it owns the corner shape while active. */
	if (client_macos_frame_active(c)) {
		client_macos_frame_radii(c, radii);
		return true;
	}

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

	e->c->crop.applied = false;
	crop_schedule();
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

/* Whether the inner hairline belongs on screen right now: the old inner
 * hairline was removed - the macOS frame owns the single inner contour now -
 * so this is always false. Kept for the geometry pass callers. */
bool
client_crop_innerline_active(Client *c)
{
	(void)c;
	return false;
}

/* The old inner hairline was removed; the macOS frame draws the single inner
 * contour. Kept as a no-op that hides any node left over from a previous
 * build or hot reload. */
void
client_crop_update_innerline(Client *c)
{
	if (c && c->crop.innerline)
		wlr_scene_node_set_enabled(&c->crop.innerline->node, false);
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

/* Deferred crop re-apply ----------------------------------------------
 *
 * Re-punching a client or layer-surface buffer touches the scene graph
 * (wlr_scene_buffer_set_buffer_with_options / set_opaque_region) and reads
 * client pixels (wlr_buffer_begin_data_ptr_access / wlr_texture_read_pixels,
 * which on DMA-BUF clients maps the DRM buffer). Running that from inside a
 * wl_surface.commit listener executes while wlroots is still walking its own
 * list of commit listeners and before the commit has finished (it re-attaches
 * the raw buffer and unlocks surface->buffer afterwards). On some drivers that
 * interleaving scribbles over the commit-signal list. Applying the crop in an
 * idle callback on the display loop runs it strictly after the dispatch that
 * triggered the commit has completed, so the scene is only ever touched from
 * outside the wayland dispatch stack.
 *
 * The idle walk only re-applies surfaces flagged stale (crop.applied == false,
 * set by the commit listeners below), so idle frames with no client commits
 * cost nothing and destroyed clients/layers are simply gone from the lists.
 */
static void
crop_reapply(void *data)
{
	Monitor *m;
	int radii[4];
	int li;

	crop_reapply_source = NULL;
	crop_reapply_pending = false;

	wl_list_for_each(m, &mons, link) {
		for (li = 0; li < (int)(sizeof(m->layers) / sizeof(m->layers[0])); li++) {
			LayerSurface *l;
			wl_list_for_each(l, &m->layers[li], link) {
				if (l->crop.applied || !l->rounded_config || !l->mapped)
					continue;
#ifdef HAVE_SCENEFX
				layer_surface_scenefx_apply_radii(l);
#else
				layer_surface_crop_apply(l);
#endif
			}
		}
	}
	foreach(ci, globalconf.clients) {
		Client *c = *ci;
		if (c->crop.applied || !client_crop_radii(c, radii))
			continue;
#ifdef HAVE_SCENEFX
		client_scenefx_apply_radii(c);
#else
		client_crop_apply(c);
#endif
	}
}

static void
crop_schedule(void)
{
	if (crop_reapply_pending)
		return;
	crop_reapply_pending = true;
	if (!crop_reapply_source)
		crop_reapply_source = wl_event_loop_add_idle(event_loop,
			crop_reapply, NULL);
}

void
cropcommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, crop_commit);

	c->crop.applied = false;
	crop_schedule();

	/* Backdrop blur re-links its transparency-mask source and re-rounds on
	 * every commit, because wlroots detaches the mask link when the buffer
	 * changes. Cheap: the blur setters no-op when nothing changed. */
	client_blur_update(c);
}

/* ========== Layer-surface corner crop (opt-in) ==========
 *
 * Layer-shell surfaces (Waybar-style bars, quickshell, layer-shell
 * notification daemons) render whatever corners they want client-side, so
 * the compositor does not round them by default. Assigning the Lua
 * layer_surface `corner_radius` property opts a surface into the shared
 * mechanism: its committed buffer gets the same true alpha punch as drawins
 * (rounded_crop_pixels), keeping the radius/arc treatment identical to
 * wiboxes and notifications. No ring, innerline or shadow is added here -
 * those stay client-side for layer surfaces.
 */

struct layer_crop_find {
	struct wlr_surface *surface;
	struct wlr_scene_buffer *found;
};

static void
layer_crop_find_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	struct layer_crop_find *f = data;
	struct wlr_scene_surface *ss;

	if (f->found)
		return;
	ss = wlr_scene_surface_try_from_buffer(buffer);
	if (ss && ss->surface == f->surface)
		f->found = buffer;
}

/* Put the raw client buffer back and drop the punched copy. */
static void
layer_surface_crop_restore(LayerSurface *l)
{
	struct layer_crop_find find;
	struct wlr_scene_buffer *sb;

	if (!l || !l->layer_surface || !l->scene)
		return;
	find = (struct layer_crop_find){ .surface = l->layer_surface->surface };
	wlr_scene_node_for_each_buffer(&l->scene->node, layer_crop_find_iter, &find);
	sb = find.found;
	if (sb && l->crop.applied && l->crop.buf && sb->buffer == l->crop.buf)
		wlr_scene_buffer_set_buffer(sb, &l->layer_surface->surface->buffer->base);
	rounded_crop_release(&l->crop);
}

void
layer_surface_crop_apply(LayerSurface *l)
{
	const rounded_config_t *cfg;
	struct wlr_surface *surface;
	struct wlr_scene_buffer *sb;
	struct layer_crop_find find;
	struct wlr_box local;
	int radii[4], i, any = 0;
	double m_ox = 0.0, m_oy = 0.0, m_sx = 1.0, m_sy = 1.0;
	double dest_w, dest_h;
	bool reuse;

	if (!l || !l->layer_surface || !l->scene)
		return;
	cfg = l->rounded_config;
	if (!cfg || !cfg->enabled) {
		layer_surface_crop_restore(l);
		return;
	}
	surface = l->layer_surface->surface;
	if (!surface || !surface->buffer)
		return;

	for (i = 0; i < 4; i++) {
		radii[i] = cfg->radii[i] < 0 ? 0 : cfg->radii[i];
		any += radii[i] > 0;
	}
	if (!any) {
		layer_surface_crop_restore(l);
		return;
	}

	find = (struct layer_crop_find){ .surface = surface };
	wlr_scene_node_for_each_buffer(&l->scene->node, layer_crop_find_iter, &find);
	sb = find.found;
	if (!sb)
		return;

	/* Buffer -> node-local mapping (same derivation as client_crop_apply so
	 * the arcs stay exact under fractional scale and viewport scaling). */
	{
		struct wlr_fbox src = sb->src_box;

		dest_w = sb->dst_width;
		dest_h = sb->dst_height;
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
	}
	if (dest_w <= 0 || dest_h <= 0)
		return;
	local = (struct wlr_box){
		.x = 0,
		.y = 0,
		.width = (int)dest_w,
		.height = (int)dest_h,
	};

	/* Any change to where the arcs fall invalidates every pixel of the
	 * previous copy, not just the surface's damage. */
	if (l->crop.buf
			&& (memcmp(l->crop.radii, radii, sizeof(l->crop.radii)) != 0
				|| l->crop.scale_x != m_sx || l->crop.scale_y != m_sy
				|| l->crop.origin_x != m_ox || l->crop.origin_y != m_oy
				|| l->crop.local.width != local.width
				|| l->crop.local.height != local.height))
		l->crop.dirty = true;

	l->crop.scale_x = m_sx;
	l->crop.scale_y = m_sy;
	l->crop.origin_x = m_ox;
	l->crop.origin_y = m_oy;
	l->crop.local = local;
	memcpy(l->crop.radii, radii, sizeof(l->crop.radii));

	/* Reuse the cached crop when the client buffer is unchanged (wlroots
	 * re-applies the raw buffer on every commit, even frame-only ones). */
	reuse = l->crop.buf
		&& l->crop.src == surface->buffer
		&& !l->crop.dirty
		&& pixman_region32_empty(&surface->buffer_damage);

	if (!reuse) {
		struct wlr_buffer *buf = NULL;
		void *data = NULL;
		uint32_t fmt = 0;
		size_t stride = 0;

		/* Read the client's pixels without going through the GPU (shm first,
		 * texture readback as fallback) - identical to the client crop. */
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
				warn("rounded corners: cannot read layer surface buffer "
					"(shm access and texture readback both failed)");
				warned = true;
			}
			return;
		}
		if (wlr_buffer_begin_data_ptr_access(buf,
				WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride)) {
			rounded_crop_pixels(data, stride, buf->width, buf->height,
				m_ox, m_oy, m_sx, m_sy, &local, radii);
			wlr_buffer_end_data_ptr_access(buf);
		}
		if (l->crop.buf)
			wlr_buffer_drop(l->crop.buf);
		l->crop.buf = buf;
		l->crop.src = surface->buffer;
	}

	{
		struct wlr_scene_buffer_set_buffer_options opts = {
			.damage = l->crop.dirty ? NULL : &surface->buffer_damage,
		};
		wlr_scene_buffer_set_buffer_with_options(sb, l->crop.buf, &opts);
	}

	/* The corners are translucent now: shrink the opaque region (which lives
	 * in buffer coordinates) by the cut corner squares so the renderer does
	 * not treat the arcs as solid. */
	{
		pixman_region32_t opaque, holes;
		const int r0 = radii[ROUNDED_TL], r1 = radii[ROUNDED_TR];
		const int r2 = radii[ROUNDED_BL], r3 = radii[ROUNDED_BR];
		const double ox = m_ox, oy = m_oy, sx = m_sx, sy = m_sy;
		pixman_box32_t boxes[4] = {
			{ (int)floor((local.x - ox) * sx), (int)floor((local.y - oy) * sy),
			  (int)ceil((local.x + r0 - ox) * sx), (int)ceil((local.y + r0 - oy) * sy) },
			{ (int)floor((local.x + local.width - r1 - ox) * sx), (int)floor((local.y - oy) * sy),
			  (int)ceil((local.x + local.width - ox) * sx), (int)ceil((local.y + r1 - oy) * sy) },
			{ (int)floor((local.x - ox) * sx), (int)floor((local.y + local.height - r2 - oy) * sy),
			  (int)ceil((local.x + r2 - ox) * sx), (int)ceil((local.y + local.height - oy) * sy) },
			{ (int)floor((local.x + local.width - r3 - ox) * sx), (int)floor((local.y + local.height - r3 - oy) * sy),
			  (int)ceil((local.x + local.width - ox) * sx), (int)ceil((local.y + local.height - oy) * sy) },
		};
		pixman_region32_init(&opaque);
		pixman_region32_copy(&opaque, &sb->opaque_region);
		pixman_region32_init_rects(&holes, boxes, 4);
		pixman_region32_subtract(&opaque, &opaque, &holes);
		wlr_scene_buffer_set_opaque_region(sb, &opaque);
		pixman_region32_fini(&holes);
		pixman_region32_fini(&opaque);
	}

	l->crop.applied = true;
	l->crop.dirty = false;
}

/* Re-apply on every surface commit (including frame-only ones): wlroots
 * re-attaches the raw buffer each time, which would otherwise undo the punch. */
void
layer_surface_cropcommitnotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, crop_commit);

	if (!l->rounded_config || !l->mapped)
		return;
	l->crop.applied = false;
	crop_schedule();
}

/* Drop the punched copy (the scene buffer dies with l->scene). */
void
layer_surface_crop_release(LayerSurface *l)
{
	if (!l)
		return;
	rounded_crop_release(&l->crop);
}

/* Global monitor list from somewm.c. */
extern struct wl_list mons;

/* Corner config changed (theme reload): re-crop every layer surface that
 * opted in via `corner_radius`. */
void
layer_surface_crop_reload(void)
{
	Monitor *m;
	int li;

	wl_list_for_each(m, &mons, link) {
		for (li = 0; li < (int)(sizeof(m->layers) / sizeof(m->layers[0])); li++) {
			LayerSurface *l;
			wl_list_for_each(l, &m->layers[li], link) {
				if (!l->rounded_config || !l->mapped)
					continue;
				l->crop.dirty = true;
				layer_surface_crop_apply(l);
			}
		}
	}
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

/* ========== SceneFX shader rounding ==========
 *
 * With SceneFX the CPU crop (client_crop_apply / layer_surface_crop_apply) is
 * replaced by shader corner radii on the scene buffers. The border ring is
 * replaced by a single frame rect whose clipped_region punches out the content
 * area. The inner hairline keeps its CPU-rendered buffer, which already traces
 * the rounded contour. The radii source of truth (client_crop_radii /
 * rounded_config) is unchanged, so the disabled build is byte-for-byte the
 * same.
 */

#ifdef HAVE_SCENEFX

static void
scenefx_apply_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	(void)sx;
	(void)sy;
	wlr_scene_buffer_set_corner_radii(buffer, *(struct fx_corner_radii *)data);
}

/* Round the content buffers and per-side titlebars with shader corner radii.
 * Re-applied on every surface commit because SceneFX resets buffer state when
 * a new buffer is attached. */
void
client_scenefx_apply_radii(Client *c)
{
	struct wlr_box inner;
	int inner_r[4];
	bool active;
	int tl, tt, tr, tb;
	struct fx_corner_radii content;

	if (!c->scene || !c->scene_surface)
		return;

	active = client_crop_inner_rect(c, &inner, inner_r);

	tl = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_LEFT].size;
	tt = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_TOP].size;
	tr = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_RIGHT].size;
	tb = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_BOTTOM].size;

	/* A content corner is square where a titlebar covers it; the bar's own
	 * buffer rounds that corner instead. */
	content = active ? corner_radii_new(
		(tl || tt) ? 0 : inner_r[ROUNDED_TL],
		(tr || tt) ? 0 : inner_r[ROUNDED_TR],
		(tr || tb) ? 0 : inner_r[ROUNDED_BR],
		(tl || tb) ? 0 : inner_r[ROUNDED_BL])
		: corner_radii_none();

	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
		scenefx_apply_iter, &content);

	/* Titlebars: each bar rounds only the corners it reaches. */
	struct fx_corner_radii tb_corners[CLIENT_TITLEBAR_COUNT] = {
		[CLIENT_TITLEBAR_TOP] = active ? corner_radii_new(
			inner_r[ROUNDED_TL], inner_r[ROUNDED_TR], 0, 0) : corner_radii_none(),
		[CLIENT_TITLEBAR_BOTTOM] = active ? corner_radii_new(
			0, 0, inner_r[ROUNDED_BR], inner_r[ROUNDED_BL]) : corner_radii_none(),
		[CLIENT_TITLEBAR_LEFT] = active ? corner_radii_new(
			tt ? 0 : inner_r[ROUNDED_TL], 0, 0, tb ? 0 : inner_r[ROUNDED_BL])
			: corner_radii_none(),
		[CLIENT_TITLEBAR_RIGHT] = active ? corner_radii_new(
			0, tt ? 0 : inner_r[ROUNDED_TR], tb ? 0 : inner_r[ROUNDED_BR], 0)
			: corner_radii_none(),
	};
	for (client_titlebar_t bar = CLIENT_TITLEBAR_TOP; bar < CLIENT_TITLEBAR_COUNT; bar++) {
		if (c->titlebar[bar].scene_buffer)
			wlr_scene_buffer_set_corner_radii(c->titlebar[bar].scene_buffer,
				tb_corners[bar]);
	}

	c->crop.applied = true;
}

/* Draw the border as one rounded frame rect with a clipped_region hole when
 * the window is rounded; otherwise fall back to the four plain border rects. */
void
client_scenefx_update_border(Client *c, int frame_w, int frame_h)
{
	int radii[4];
	bool active = client_crop_radii(c, radii);
	bool want = active && c->bw > 0;

	if (!want) {
		if (c->border_frame)
			wlr_scene_node_set_enabled(&c->border_frame->node, false);
		return;
	}

	if (!c->border_frame) {
		c->border_frame = wlr_scene_rect_create(c->scene, 0, 0, c->border[0]->color);
		if (!c->border_frame)
			return;
		c->border_frame->node.data = c->scene->node.data;
		wlr_scene_node_place_above(&c->border_frame->node, &c->border[3]->node);
	}

	/* Border rects collapse to nothing; the frame rect draws the border. */
	for (int i = 0; i < 4; i++)
		wlr_scene_rect_set_size(c->border[i], 0, 0);

	int inner[4];
	struct wlr_box hole;
	hole = (struct wlr_box){ .x = c->bw, .y = c->bw,
		.width = c->geometry.width, .height = c->geometry.height };
	for (int i = 0; i < 4; i++) {
		inner[i] = radii[i] - c->bw;
		if (inner[i] < 0)
			inner[i] = 0;
	}

	wlr_scene_rect_set_size(c->border_frame, frame_w, frame_h);
	wlr_scene_node_set_position(&c->border_frame->node, 0, 0);
	wlr_scene_rect_set_corner_radii(c->border_frame, corner_radii_new(
		radii[ROUNDED_TL], radii[ROUNDED_TR],
		radii[ROUNDED_BR], radii[ROUNDED_BL]));
	wlr_scene_rect_set_clipped_region(c->border_frame, (struct clipped_region){
		.area = hole,
		.corners = corner_radii_new(inner[ROUNDED_TL], inner[ROUNDED_TR],
			inner[ROUNDED_BR], inner[ROUNDED_BL]),
	});
	wlr_scene_node_set_enabled(&c->border_frame->node, true);
}

/* Round the layer surface's buffers with shader corner radii. Re-applied on
 * every commit; the layer surface also carries a SceneFX blur node (below),
 * which is separate from these buffer radii. */
void
layer_surface_scenefx_apply_radii(LayerSurface *l)
{
	const rounded_config_t *cfg;
	struct fx_corner_radii corners;

	if (!l || !l->layer_surface || !l->scene)
		return;

	cfg = l->rounded_config;
	if (!cfg || !cfg->enabled) {
		corners = corner_radii_none();
	} else {
		corners = corner_radii_new(cfg->radii[ROUNDED_TL], cfg->radii[ROUNDED_TR],
			cfg->radii[ROUNDED_BR], cfg->radii[ROUNDED_BL]);
	}

	wlr_scene_node_for_each_buffer(&l->scene->node, scenefx_apply_iter, &corners);
	l->crop.applied = true;
}

#endif /* HAVE_SCENEFX */

/* ========== SceneFX backdrop blur ==========
 *
 * wlroots 0.4's wlr_scene_buffer_set_backdrop_blur() is gone in SceneFX 0.5:
 * backdrop blur is now a scene node (wlr_scene_blur) that blurs everything
 * painted behind it. The node tracks the object's content box and corner
 * radii and takes the content buffer as a transparency-mask source, so the
 * blur only shows where the surface actually paints (transparent pixels stay
 * sharp). Re-applied on every surface commit because wlroots detaches the
 * mask link and resets buffer state then. All scene access is inside blur.c
 * (stubbed in a -Dscenefx=disabled build), so these drivers are build-safe.
 */

/* Content buffer serving as the blur's transparency mask. */
static struct wlr_scene_buffer *
blur_client_mask(Client *c)
{
	struct client_crop_find find = {
		.surface = window_client_surface(c),
	};
	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
		client_crop_find_iter, &find);
	return find.found;
}

static struct wlr_scene_buffer *
blur_layer_mask(LayerSurface *l)
{
	struct layer_crop_find find = {
		.surface = l->layer_surface->surface,
	};
	wlr_scene_node_for_each_buffer(&l->scene->node,
		layer_crop_find_iter, &find);
	return find.found;
}

/* Fit or destroy the client's blur node. The node lives inside c->scene at
 * the content box (frame coordinates), rounded to the same corners as the
 * content crop and masked by the content buffer. */
void
client_blur_update(Client *c)
{
	struct wlr_box area;
	int inner_r[4];
	int radii[4];
	struct wlr_scene_buffer *mask;
	int tl, tt;

	if (!c->scene || !c->scene_surface || !window_client_has_surface(c))
		return;

	/* Off while fullscreen, unattached, or configured off. */
	if (!c->blur_config || !c->blur_config->enabled || c->fullscreen
			|| !window_client_surface(c)->mapped) {
		blur_release(&c->blur);
		return;
	}

	/* Content box in frame coordinates: inset by the border, offset by the
	 * titlebars that occupy geometry (same derivation as the content
	 * corner radii in client_scenefx_apply_radii). */
	client_crop_inner_rect(c, &area, inner_r);
	if (!client_crop_radii(c, radii)) {
		inner_r[0] = inner_r[1] = inner_r[2] = inner_r[3] = 0;
	}
	tl = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_LEFT].size;
	tt = c->fullscreen ? 0 : c->titlebar[CLIENT_TITLEBAR_TOP].size;
	area.x += c->bw + tl;
	area.y += c->bw + tt;

	mask = blur_client_mask(c);
	blur_apply(c->scene, &c->blur, c->blur_config, &area, inner_r, mask);
	/* Blur tracks the window's own opacity, so commit-time re-application
	 * needs no Lua coordination during fades. */
	blur_set_fade(&c->blur,
		c->opacity >= 0 ? (float)c->opacity : 1.0f);

	/* Blur the backdrop, then paint the surface on top, never over it.
	 * The node has to sit below content in the frame tree. */
#ifdef HAVE_SCENEFX
	if (c->blur.node) {
		wlr_scene_node_place_below(&c->blur.node->node,
			&c->scene_surface->node);
	}
#endif
}

/* Release a layer surface's blur nodes (backdrop node plus the SceneFX
 * optimized cache node). */
static void
layer_surface_blur_release(LayerSurface *l)
{
#ifdef HAVE_SCENEFX
	if (l->bg_blur_optimized) {
		wlr_scene_node_destroy(&l->bg_blur_optimized->node);
		l->bg_blur_optimized = NULL;
	}
#endif
	blur_release(&l->blur);
}

/* Fit or destroy the blur node driven by ext-background-effect-v1. The panel
 * blur is a plain rectangle (no corners, no transparency mask) backed by the
 * SceneFX optimized blur cache. */
static void
layer_surface_bg_blur_update(LayerSurface *l)
{
	static const blur_config_t config = {
		.enabled = true,
		.corner_radius = 0,
		.alpha = 1.0f,
		.strength = 1.0f,
	};
	struct wlr_box area;
	pixman_box32_t *extents;
	int sw, sh;

	if (!l->mapped || pixman_region32_empty(&l->bg_blur_region)) {
		layer_surface_blur_release(l);
		return;
	}

	extents = pixman_region32_extents(&l->bg_blur_region);
	sw = l->layer_surface->current.actual_width
		? l->layer_surface->current.actual_width
		: l->layer_surface->current.desired_width;
	sh = l->layer_surface->current.actual_height
		? l->layer_surface->current.actual_height
		: l->layer_surface->current.desired_height;

	/* The protocol region is surface-local; clip it to the surface size. */
	area = (struct wlr_box){
		.x = extents->x1,
		.y = extents->y1,
		.width = extents->x2 - extents->x1,
		.height = extents->y2 - extents->y1,
	};
	if (area.x < 0) {
		area.width += area.x;
		area.x = 0;
	}
	if (area.y < 0) {
		area.height += area.y;
		area.y = 0;
	}
	if (area.x + area.width > sw)
		area.width = sw - area.x;
	if (area.y + area.height > sh)
		area.height = sh - area.y;
	if (area.width <= 0 || area.height <= 0) {
		layer_surface_blur_release(l);
		return;
	}

	blur_apply(l->scene, &l->blur, &config, &area, NULL, NULL);
	blur_set_only_bottom_layer(&l->blur, true);
	blur_set_fade(&l->blur,
		l->opacity >= 0 ? (float)l->opacity : 1.0f);

#ifdef HAVE_SCENEFX
	if (l->blur.node) {
		/* The optimized node re-renders the cached blurred wallpaper only
		 * when marked dirty; it sits below the blur node so the cache is
		 * refreshed before the blur samples it. */
		if (!l->bg_blur_optimized)
			l->bg_blur_optimized = wlr_scene_optimized_blur_create(l->scene,
				area.width, area.height);
		if (l->bg_blur_optimized) {
			wlr_scene_optimized_blur_set_size(l->bg_blur_optimized,
				area.width, area.height);
			wlr_scene_node_set_position(&l->bg_blur_optimized->node,
				area.x, area.y);
			wlr_scene_node_place_below(&l->bg_blur_optimized->node,
				&l->blur.node->node);
		}
		wlr_scene_node_lower_to_bottom(&l->blur.node->node);
	}
#endif
}

/* Fit or destroy a layer surface's blur node. */
void
layer_surface_blur_update(LayerSurface *l)
{
	const rounded_config_t *rconfig;
	struct wlr_box area;
	struct wlr_scene_buffer *mask;
	int radii[4];
	int i;

	if (!l || !l->scene || !l->layer_surface)
		return;

	/* The protocol-driven panel blur wins over the Lua backdrop_blur. */
	if (l->bg_blur_enabled) {
		layer_surface_bg_blur_update(l);
		return;
	}

	if (!l->blur_config || !l->blur_config->enabled || !l->mapped) {
		layer_surface_blur_release(l);
		return;
	}

	rconfig = l->rounded_config;
	for (i = 0; i < 4; i++)
		radii[i] = (rconfig && rconfig->enabled) ? rconfig->radii[i] : 0;
	for (i = 0; i < 4; i++) {
		if (radii[i] < 0)
			radii[i] = 0;
	}

	/* Full surface box in node-local coordinates. */
	area = (struct wlr_box){
		.x = 0,
		.y = 0,
		.width = l->layer_surface->current.actual_width
			? l->layer_surface->current.actual_width
			: l->layer_surface->current.desired_width,
		.height = l->layer_surface->current.actual_height
			? l->layer_surface->current.actual_height
			: l->layer_surface->current.desired_height,
	};

	mask = blur_layer_mask(l);
	blur_apply(l->scene, &l->blur, l->blur_config, &area, radii, mask);
	/* Same opacity tracking as clients (see above). */
	blur_set_fade(&l->blur,
		l->opacity >= 0 ? (float)l->opacity : 1.0f);

	/* Blur the backdrop, then paint the surface on top: keep the node below
	 * the layer's whole surface subtree. lower_to_bottom needs no sibling,
	 * which is safer here: the mask buffer sits under intermediate trees. */
#ifdef HAVE_SCENEFX
	if (l->blur.node) {
		wlr_scene_node_lower_to_bottom(&l->blur.node->node);
	}
#endif
}

/* ========== Opacity-correct fades ==========
 *
 * Buffers fade through wlr_scene_buffer_set_opacity (see
 * client_apply_opacity_to_scene); decorations follow through their own
 * alpha channels so one Lua tick on `c.opacity` fades the whole window:
 * border rects (plain wlroots rects blend color alpha in both builds),
 * the shadow (GPU node color alpha, or nine-patch buffer opacity without
 * SceneFX) and the backdrop blur (set_alpha + set_strength). No
 * hide-during-fade is needed: unlike SceneFX 0.4, 0.5 blends rect and
 * shadow color alpha live.
 */

/* Set every buffer in a scene (sub)tree to one opacity. Rect, shadow and
 * blur nodes are skipped: they are decorations with their own alpha. */
void
scene_apply_opacity(struct wlr_scene_node *node, float opacity)
{
	struct wlr_scene_node *child;

	if (!node)
		return;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node),
			opacity);
		return;
	}
	if (node->type != WLR_SCENE_NODE_TREE)
		return;
	wl_list_for_each(child,
		&wlr_scene_tree_from_node(node)->children, link)
		scene_apply_opacity(child, opacity);
}

/* Record a new unfaded border color (focus flip, Lua border_color, theme
 * default) and immediately re-apply it scaled by the current opacity. */
void
client_border_set_base(Client *c, const float color[static 4])
{
	if (!c)
		return;
	memcpy(c->border_color_base, color, sizeof(c->border_color_base));
	c->border_color_base_set = true;
	client_fade_apply(c, c->opacity >= 0 ? (float)c->opacity : 1.0f);
}

/* Fade a client's decorations with its buffer opacity (0..1). */
void
client_fade_apply(Client *c, float opacity)
{
	float floats[4];
	int i;

	if (!c || !c->scene || !c->border[0])
		return;
	if (opacity < 0.0f)
		opacity = 0.0f;
	if (opacity > 1.0f)
		opacity = 1.0f;

	/* Backdrop blur tracks through its own alpha/strength. */
	blur_set_fade(&c->blur, opacity);

	/* Border rects blend their color alpha live. The base color may come
	 * from Lua (border_color.initialized) or the theme default, so fade
	 * whatever base is recorded, not just the Lua-set one. */
	if (c->border_color_base_set) {
		memcpy(floats, c->border_color_base, sizeof(floats));
		floats[3] *= opacity;
		for (i = 0; i < 4; i++)
			wlr_scene_rect_set_color(c->border[i], floats);
#ifdef HAVE_SCENEFX
		if (c->border_frame)
			wlr_scene_rect_set_color(c->border_frame, floats);
#endif
	}

	/* Shadow follows through its own alpha or buffer opacity. */
	shadow_set_fade(&c->shadow, opacity);

	/* Native macOS frame: stroke/highlight buffer opacity + shadow alpha. */
	client_macos_frame_set_fade(c, opacity);
}

