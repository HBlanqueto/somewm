/*
 * slide.c - macOS-style horizontal tag-switch slide
 *
 * When a tag switch goes from exactly one selected tag to exactly one other
 * tag on the same screen, the outgoing desktop slides out and the incoming
 * one slides in. The move is purely scene-graph: every sliding client gets a
 * per-client visual_offset_x (the same mechanism the focus-mode reveal uses
 * for Y) and each desktop gets its own backdrop node, so nothing is ever
 * resized or reconfigured.
 *
 * Ticks are advanced from output frame events (rendermon) for vsync-aligned
 * motion, with a 1 ms timer + some_refresh() as a time-based fallback. Both
 * compute the same absolute progress, so calling both is idempotent.
 *
 * Focus-space tags (tag.backdrop = "black") additionally get a persistent
 * solid-black rect in LyrBg whenever they are selected, independent of any
 * animation, so the black backdrop also holds when the slide is disabled or
 * the switch is instant.
 */

#include "slide.h"

#include "animation.h"
#include "globalconf.h"
#include "luaa.h"
#include "objects/client.h"
#include "objects/drawable.h"
#include "objects/screen.h"
#include "somewm_api.h"
#include "somewm_types.h"
#include "scenefx_compat.h"
#include "window.h"

#include <cairo.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

/* Scene layers (defined in somewm.c) */
extern struct wlr_scene_tree *layers[NUM_LAYERS];
/* Monitor list (defined in somewm.c) */
extern struct wl_list mons;
extern Monitor *selmon;
/* Focus helpers (somewm.c) */
extern void focusclient(Client *c, int lift);
extern Client *focustop(Monitor *m);
extern void motionnotify(uint32_t time, struct wlr_input_device *device,
	double sx, double sy, double sx_unaccel, double sy_unaccel);
/* somewm.c helpers that toggle the root background rect (static there) */
extern void some_slide_set_root_bg_visible(bool visible);
extern bool some_slide_root_bg_visible(void);

/* ========================================================================
 * Configuration (defaults, Lua-settable via the `slide` global)
 * ======================================================================== */

static bool slide_enabled = true;
static double slide_duration = 0.300;
static int slide_easing = EASING_EASE_OUT_CUBIC;
static int slide_gap = 80;
static float slide_gap_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

void
slide_set_enabled(bool enabled)
{
	slide_enabled = enabled;
}

void
slide_set_duration(double seconds)
{
	if (seconds > 0)
		slide_duration = seconds;
}

void
slide_set_easing(int easing)
{
	slide_easing = easing;
}

void
slide_set_gap(int px)
{
	if (px >= 0)
		slide_gap = px;
}

void
slide_set_gap_color(const float rgba[4])
{
	memcpy(slide_gap_color, rgba, sizeof(slide_gap_color));
}

/* ========================================================================
 * Sliding layer surfaces (opt-in by layer-shell namespace)
 *
 * Only layer surfaces whose namespace is registered here (e.g. the bar)
 * slide with the desktops: the real, live surface node moves with the
 * incoming desktop while a FROZEN copy of its current buffers slides out
 * with the outgoing desktop. The notch, dock and helper surfaces are never
 * matched, so they stay fixed.
 * ======================================================================== */

#define SLIDE_MAX_LAYER_NS 8
static char *slide_layer_ns[SLIDE_MAX_LAYER_NS];
static int slide_layer_ns_count;

void
slide_set_sliding_layers(const char *const *namespaces, int count)
{
	int i;
	for (i = 0; i < slide_layer_ns_count; i++)
		free(slide_layer_ns[i]);
	slide_layer_ns_count = 0;
	if (count > SLIDE_MAX_LAYER_NS)
		count = SLIDE_MAX_LAYER_NS;
	for (i = 0; i < count; i++)
		slide_layer_ns[slide_layer_ns_count++] = strdup(namespaces[i]);
}

static bool
slide_layer_ns_match(const char *ns)
{
	int i;
	if (!ns)
		return false;
	for (i = 0; i < slide_layer_ns_count; i++)
		if (slide_layer_ns[i] && strcmp(slide_layer_ns[i], ns) == 0)
			return true;
	return false;
}

/* ========================================================================
 * Previous-selection snapshot (drives the 1->1 detection)
 * ======================================================================== */

static int tag_lua_index(tag_t *t);

struct prev_sel {
	Monitor *m;
	tag_t *tag;   /* may dangle once a tag is deleted; never dereference it
	               * unless tag_is_alive(); index/backdrop are snapshots */
	int index;
	int backdrop;
};

#define MAX_PREV_SEL 64
static struct prev_sel prev_sel[MAX_PREV_SEL];
static int prev_sel_count;

static struct prev_sel *
prev_selected_get(Monitor *m)
{
	for (int i = 0; i < prev_sel_count; i++)
		if (prev_sel[i].m == m)
			return &prev_sel[i];
	return NULL;
}

static void
prev_selected_set(Monitor *m, tag_t *t)
{
	int index = t ? tag_lua_index(t) : -1;
	int backdrop = t ? t->backdrop : TAG_BACKDROP_WALLPAPER;
	for (int i = 0; i < prev_sel_count; i++) {
		if (prev_sel[i].m == m) {
			prev_sel[i].tag = t;
			prev_sel[i].index = index;
			prev_sel[i].backdrop = backdrop;
			return;
		}
	}
	if (prev_sel_count < MAX_PREV_SEL) {
		prev_sel[prev_sel_count].m = m;
		prev_sel[prev_sel_count].tag = t;
		prev_sel[prev_sel_count].index = index;
		prev_sel[prev_sel_count].backdrop = backdrop;
		prev_sel_count++;
	}
}

static void
prev_selected_clear_all(void)
{
	prev_sel_count = 0;
}

/* Exactly one tag selected on this monitor? Returns it, or NULL. */
static tag_t *
single_selected_on(Monitor *m)
{
	tag_t *found = NULL;
	for (int i = 0; i < globalconf.tags.len; i++) {
		tag_t *t = globalconf.tags.tab[i];
		if (!t || !t->selected)
			continue;
		if (!t->screen || t->screen->monitor != m)
			continue;
		if (found)
			return NULL; /* more than one selected */
		found = t;
	}
	return found;
}

/* A tag is only usable by the slide while it is still present in
 * globalconf.tags (which holds a strong reference, so membership means the
 * memory is live) and activated. Focus-space temp tags are deleted
 * (activated=false, removed from the array) the moment their window leaves,
 * which can happen before the deferred banning_refresh() that would start the
 * slide runs. A dead tag's tag_t may already be freed, so it must never be
 * dereferenced: the membership scan is a pointer comparison, and only a tag
 * found in the array gets its ->activated field read. */
static bool
tag_is_alive(tag_t *t)
{
	if (!t)
		return false;
	for (int i = 0; i < globalconf.tags.len; i++)
		if (globalconf.tags.tab[i] == t)
			return t->activated;
	return false;
}

/* ========================================================================
 * Active slide state
 * ======================================================================== */

struct slide_desktop {
	tag_t *tag;
	bool black;                    /* backdrop is solid black (focus space) */
	struct wlr_scene_buffer *wallpaper; /* wallpaper crop, or NULL */
	struct wlr_scene_rect *black_bg;    /* solid black rect, or NULL */
	client_array_t clients;
};

#define SLIDE_MAX_STATIC 16

#define SLIDE_MAX_LAYERS 8
struct slide_layer {
	LayerSurface *l;                  /* the sliding layer surface */
	struct wlr_scene_tree *frozen;    /* frozen copy of its buffers, or NULL */
	int anchor_x, anchor_y;           /* last known arranged anchor */
	struct wl_listener surface_destroy; /* clears ->l if the surface dies */
};

/* One wallpaper scene node the slide hid at start, with the exact enabled
 * state it had then. A destroy listener clears ->node if the node is replaced
 * or destroyed mid-slide (wallpaper change), so teardown never touches a freed
 * node and never re-enables a node the wallpaper system superseded. */
#define SLIDE_MAX_WP_NODES (1 + WALLPAPER_MAX_SCREENS) /* legacy + per-screen */
struct slide_wp_node_hidden {
	struct wlr_scene_buffer *node;   /* cleared by the destroy listener */
	bool enabled;                    /* enabled state at slide start */
	bool is_legacy;                  /* legacy node vs per-screen cache node */
	int screen_index;                /* cache nodes: 0-based screen index */
	struct wl_listener destroy;
};

#define SLIDE_MAX_WP_SOURCES WALLPAPER_MAX_SCREENS

/* Slide-owned snapshot of the current wallpaper for one monitor: an SHM
 * wlr_buffer we created ourselves (so releasing it is wlr_buffer_drop(), which
 * is ours to call), built from whatever cairo surface is actually visible on
 * that screen (the per-screen cache surface for filepath wallpapers, else
 * globalconf.wallpaper for the legacy path). Rebuilt only when the wallpaper
 * changes: the fingerprint is the source surface plus the monitor box, and
 * slide_wallpaper_changed() drops every buffer on root.wallpaper / cache show
 * / reload wipe. Every slide backdrop crop locks this buffer, so a slide
 * always carries the real wallpaper regardless of the scene node's lifecycle. */
struct slide_wp_source {
	Monitor *m;
	cairo_surface_t *surface;       /* fingerprint: cairo surface built from */
	wallpaper_cache_entry_t *entry; /* fingerprint: cache entry built from */
	int origin_x, origin_y;         /* buffer origin in layout coordinates */
	int w, h;                       /* buffer size */
	struct wlr_buffer *buffer;      /* owned reference */
};

struct slide_state {
	bool active;
	Monitor *mon;
	tag_t *old_tag;
	tag_t *new_tag;
	int direction;         /* +1 new to the right (slide left), -1 slide right */
	int distance;          /* monitor width + gap */
	double t0;
	double duration;
	int easing;
	double eased;          /* last applied (eased) progress */
	bool manual;           /* set_progress() drives instead of the clock */
	struct slide_desktop out;
	struct slide_desktop in;
	struct wlr_scene_rect *gap_rect;
	struct wlr_scene_buffer *static_bg[SLIDE_MAX_STATIC];
	int static_bg_count;
	struct slide_wp_node_hidden hidden[SLIDE_MAX_WP_NODES];
	int hidden_count;
	bool root_bg_visible;
	bool black_warned;     /* one clear warning per slide on a black degrade */
	struct slide_layer layers[SLIDE_MAX_LAYERS];
	int layers_count;
	struct wl_event_source *timer; /* 1 ms fallback driver */
	int frames;  /* tick frames of the current (or last) slide */
	double last_apply;  /* monotonic time of the last applied frame */
	double last_frame_apply;  /* monotonic time of the last FRAME-driven apply */
};

static struct slide_state slide;

static struct slide_wp_source slide_wp_sources[SLIDE_MAX_WP_SOURCES];

static double
clock_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ========================================================================
 * Backdrops
 * ======================================================================== */

/* Copy the (sx,sy,w,h) region of a cairo surface into a new SHM wlr_buffer.
 * Ownership of the returned buffer passes to the caller. */
static struct wlr_buffer *
slide_wp_buffer_region(cairo_surface_t *src, int sx, int sy, int w, int h)
{
	struct wlr_buffer *buf = NULL;
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);

	if (cairo_surface_status(tmp) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(tmp);
		return NULL;
	}
	cairo_t *cr = cairo_create(tmp);
	if (cr) {
		cairo_set_source_surface(cr, src, -sx, -sy);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_destroy(cr);
	}
	cairo_surface_flush(tmp);
	buf = drawable_create_buffer_from_data(w, h,
		cairo_image_surface_get_data(tmp), cairo_image_surface_get_stride(tmp));
	cairo_surface_destroy(tmp);
	return buf;
}

/* 0-based index of monitor m into globalconf.screens /
 * globalconf.current_wallpaper_per_screen, or -1 when unknown. */
static int
slide_monitor_screen_index(Monitor *m)
{
	for (int i = 0; i < globalconf.screens.len; i++) {
		screen_t *s = globalconf.screens.tab[i];
		if (s && s->monitor == m)
			return s->index - 1;
	}
	return -1;
}

/* Return (and build on first use or wallpaper change) the slide-owned
 * wallpaper buffer covering monitor m, or NULL when no wallpaper surface is
 * available. Never rebuilt per slide: only when the source surface or the
 * monitor box changed, or after slide_wallpaper_changed(). */
static struct slide_wp_source *
slide_wp_ensure(Monitor *m)
{
	struct slide_wp_source *src = NULL;
	cairo_surface_t *src_surface;
	wallpaper_cache_entry_t *entry = NULL;
	int idx = slide_monitor_screen_index(m);
	int ex = 0, ey = 0;   /* entry surface origin in layout coordinates */

	/* The visible wallpaper for a screen is the most recently applied one.
	 * A legacy (root.wallpaper) application creates globalconf.wallpaper and
	 * the legacy node; a later per-screen cache show destroys that node, so a
	 * live legacy node means the legacy surface is still what is shown and
	 * any cache entry left current is stale. Only when the legacy node is
	 * gone (the cache path took over) is the per-screen cache surface used. */
	if (idx >= 0 && idx < WALLPAPER_MAX_SCREENS && !globalconf.wallpaper_buffer_node)
		entry = globalconf.current_wallpaper_per_screen[idx];
	if (entry && entry->surface) {
		src_surface = entry->surface;
		if (idx >= 0 && idx < globalconf.screens.len) {
			screen_t *s = globalconf.screens.tab[idx];
			if (s) {
				ex = s->geometry.x;
				ey = s->geometry.y;
			}
		}
	} else {
		entry = NULL;
		if (!globalconf.wallpaper)
			return NULL;
		src_surface = globalconf.wallpaper;
	}
	if (m->m.width <= 0 || m->m.height <= 0)
		return NULL;

	for (int i = 0; i < SLIDE_MAX_WP_SOURCES; i++)
		if (slide_wp_sources[i].m == m) {
			src = &slide_wp_sources[i];
			break;
		}
	if (!src)
		for (int i = 0; i < SLIDE_MAX_WP_SOURCES; i++)
			if (!slide_wp_sources[i].m) {
				src = &slide_wp_sources[i];
				break;
			}
	if (!src)
		return NULL;

	/* Already built for this monitor with the current wallpaper? */
	if (src->buffer && src->surface == src_surface && src->entry == entry
			&& src->origin_x == m->m.x && src->origin_y == m->m.y
			&& src->w == m->m.width && src->h == m->m.height)
		return src;

	/* Wallpaper or geometry changed: rebuild (drop our owned reference). */
	if (src->buffer) {
		wlr_buffer_drop(src->buffer);
		src->buffer = NULL;
	}
	struct wlr_buffer *buf = slide_wp_buffer_region(src_surface,
		m->m.x - ex, m->m.y - ey, m->m.width, m->m.height);
	if (!buf)
		return NULL;
	src->buffer = buf;
	src->surface = src_surface;
	src->entry = entry;
	src->origin_x = m->m.x;
	src->origin_y = m->m.y;
	src->w = m->m.width;
	src->h = m->m.height;
	return src;
}

/* Drop every slide-owned wallpaper buffer: the wallpaper changed (root.wallpaper
 * / cache show / reload wipe) or the compositor is going away. Any crop scene
 * node keeps its own buffer lock and survives until destroyed. */
void
slide_wallpaper_changed(void)
{
	for (int i = 0; i < SLIDE_MAX_WP_SOURCES; i++) {
		struct slide_wp_source *src = &slide_wp_sources[i];
		if (src->buffer) {
			wlr_buffer_drop(src->buffer);
			src->buffer = NULL;
		}
		memset(src, 0, sizeof(*src));
	}
}

/* A wlr_scene_buffer showing `box` (layout coordinates) of the slide-owned
 * wallpaper buffer covering `src`'s monitor. The scene buffer locks the buffer
 * for its lifetime, so the crop stays alive even if the slide-owned buffer is
 * later rebuilt; it must NOT be locked or dropped here (the wallpaper paths
 * own those). */
static struct wlr_scene_buffer *
wallpaper_crop_from_source(struct slide_wp_source *src, struct wlr_box *box)
{
	struct wlr_scene_buffer *node;
	struct wlr_fbox sb;

	if (!src || !src->buffer)
		return NULL;
	node = wlr_scene_buffer_create(layers[LyrBg], src->buffer);
	if (!node)
		return NULL;
	sb = (struct wlr_fbox) {
		.x = box->x - src->origin_x,
		.y = box->y - src->origin_y,
		.width = box->width,
		.height = box->height,
	};
	wlr_scene_buffer_set_source_box(node, &sb);
	wlr_scene_buffer_set_dest_size(node, box->width, box->height);
	wlr_scene_node_set_position(&node->node, box->x, box->y);
	return node;
}

/* Fallback crop directly from the shared wallpaper node's buffer (layout
 * coordinates), used only when no slide-owned source is available. Same
 * locking rules as above. */
static struct wlr_scene_buffer *
wallpaper_crop_from_node(struct wlr_scene_buffer *node, struct wlr_box *box)
{
	struct wlr_scene_buffer *crop;
	struct wlr_fbox sb;

	if (!node || !node->buffer)
		return NULL;
	crop = wlr_scene_buffer_create(layers[LyrBg], node->buffer);
	if (!crop)
		return NULL;
	sb = (struct wlr_fbox) {
		.x = box->x, .y = box->y,
		.width = box->width, .height = box->height,
	};
	wlr_scene_buffer_set_source_box(crop, &sb);
	wlr_scene_buffer_set_dest_size(crop, box->width, box->height);
	wlr_scene_node_set_position(&crop->node, box->x, box->y);
	return crop;
}

static struct wlr_scene_rect *
black_rect_create(struct wlr_box *box)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	struct wlr_scene_rect *rect =
		wlr_scene_rect_create(layers[LyrBg], box->width, box->height, black);
	if (!rect)
		return NULL;
	wlr_scene_node_set_position(&rect->node, box->x, box->y);
	return rect;
}

/* The wallpaper scene node the slide hid was destroyed or replaced mid-slide
 * (a wallpaper change): clear our pointer so teardown does not touch it. The
 * signal list is going away, so remove ourselves here; teardown checks
 * wl_list_empty() before removing again. */
static void
slide_wp_node_destroy(struct wl_listener *listener, void *data)
{
	struct slide_wp_node_hidden *h = wl_container_of(listener, h, destroy);
	(void)data;
	wl_list_remove(&h->destroy.link);
	wl_list_init(&h->destroy.link);
	h->node = NULL;
}

/* Record node's exact enabled state, hide it, and track it for exact restore
 * at teardown (a no-op if the node dies mid-slide). */
static void
slide_hide_wp_node(struct wlr_scene_buffer *node, bool is_legacy, int screen_index)
{
	struct slide_wp_node_hidden *h;

	if (!node || slide.hidden_count >= SLIDE_MAX_WP_NODES)
		return;
	h = &slide.hidden[slide.hidden_count++];
	h->node = node;
	h->is_legacy = is_legacy;
	h->screen_index = screen_index;
	h->enabled = node->node.enabled;
	h->destroy.notify = slide_wp_node_destroy;
	wl_signal_add(&node->node.events.destroy, &h->destroy);
	wlr_scene_node_set_enabled(&node->node, false);
}

/* Build the backdrop node for one desktop (called from slide_start). `kind`
 * is the tag backdrop kind; the outgoing desktop may come from a snapshot
 * when its tag was deleted, so it is passed explicitly rather than read off a
 * (possibly dangling) tag pointer. Always clears both node slots so a stale
 * pointer from a previous slide can never be destroyed twice.
 *
 * Fallback order: the slide-owned wallpaper snapshot → the currently visible
 * wallpaper scene node's buffer → solid black. A black degrade logs ONE clear
 * warning per slide, so it can never happen silently again. */
static void
desktop_backdrop_setup(struct slide_desktop *d, int kind, Monitor *m, struct wlr_box *box)
{
	struct slide_wp_source *src;

	d->wallpaper = NULL;
	d->black_bg = NULL;
	d->black = (kind == TAG_BACKDROP_BLACK);
	if (d->black) {
		d->black_bg = black_rect_create(box);
		return;
	}

	src = slide_wp_ensure(m);
	if (src)
		d->wallpaper = wallpaper_crop_from_source(src, box);
	if (!d->wallpaper && globalconf.wallpaper_buffer_node)
		d->wallpaper = wallpaper_crop_from_node(globalconf.wallpaper_buffer_node, box);
	if (!d->wallpaper) {
		d->black_bg = black_rect_create(box);
		if (!slide.black_warned) {
			fprintf(stderr, "somewm: slide: WARNING: no wallpaper backdrop "
				"available for a desktop (slide-owned snapshot and scene "
				"node missing); falling back to solid black\n");
			slide.black_warned = true;
		}
	}
}

/* ========================================================================
 * Applying progress
 * ======================================================================== */

static void
slide_apply_desktop(struct slide_desktop *d, int offset)
{
	for (int i = 0; i < d->clients.len; i++) {
		Client *c = d->clients.tab[i];
		if (!c || !c->scene)
			continue;
		c->visual_offset_x = offset;
		client_apply_scene_offset(c);
	}
	if (d->wallpaper)
		wlr_scene_node_set_position(&d->wallpaper->node,
			slide.mon->m.x + offset, slide.mon->m.y);
	if (d->black_bg)
		wlr_scene_node_set_position(&d->black_bg->node,
			slide.mon->m.x + offset, slide.mon->m.y);
}

static void slide_apply_layers(int off_out, int off_in);

static void
slide_apply(double eased)
{
	int off_out = -slide.direction * (int)llround(eased * slide.distance);
	int off_in = slide.direction * (int)llround((1.0 - eased) * slide.distance);

	slide.frames++;

	slide_apply_desktop(&slide.out, off_out);
	slide_apply_desktop(&slide.in, off_in);
	slide_apply_layers(off_out, off_in);

	/* The gap strip travels with the outgoing desktop's right edge, staying
	 * exactly `slide_gap` px between the two desktops for the whole slide. */
	if (slide.gap_rect) {
		wlr_scene_node_set_position(&slide.gap_rect->node,
			slide.mon->m.x + slide.mon->m.width + off_out, slide.mon->m.y);
	}

	/* Pointer focus follows the moving desktops: input-blocked outgoing
	 * clients are skipped by the scene hit-test, so the incoming desktop (or
	 * the fixed layer-shell surfaces) gets the pointer. */
	motionnotify(0, NULL, 0, 0, 0, 0);
}

/* ========================================================================
 * Sliding layer surfaces (the bar): real node moves in, frozen copy goes out
 * ======================================================================== */

/* The frozen copy must never take input: wlr_scene_node_at() skips buffers
 * whose point_accepts_input callback rejects the point. */
static bool
slide_frozen_reject_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer; (void)sx; (void)sy;
	return false;
}

/* A layer surface's scene tree (l->scene) can hold the main surface buffer
 * plus subsurfaces (Qt case). Clone every wlr_scene_buffer node into a new
 * tree at the same relative position, sharing (locking) the same wlr_buffer:
 * wlr_scene_buffer_create() takes its own reference, so the copy stays alive
 * and shows the captured frame even if the client swaps its buffer, and
 * destroying the copy node releases the reference exactly once. We never call
 * wlr_buffer_drop() on a buffer we do not own. */
static void
slide_layer_frozen_clone(struct wlr_scene_node *src, struct wlr_scene_tree *dst,
		int ox, int oy)
{
	struct wlr_scene_node *child;
	if (src->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(src);
		wl_list_for_each(child, &tree->children, link)
			slide_layer_frozen_clone(child, dst, ox + src->x, oy + src->y);
		return;
	}
	if (src->type != WLR_SCENE_NODE_BUFFER)
		return;
	{
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(src);
		struct wlr_scene_buffer *copy;
		if (!sb || !sb->buffer)
			return;
		copy = wlr_scene_buffer_create(dst, sb->buffer); /* takes its own lock */
		if (!copy)
			return;
		copy->src_box = sb->src_box;
		wlr_scene_buffer_set_dest_size(copy, sb->dst_width, sb->dst_height);
		wlr_scene_buffer_set_transform(copy, sb->transform);
		wlr_scene_buffer_set_opacity(copy, sb->opacity);
		wlr_scene_buffer_set_filter_mode(copy, sb->filter_mode);
		copy->point_accepts_input = slide_frozen_reject_input;
		wlr_scene_node_set_position(&copy->node, ox + src->x, oy + src->y);
	}
}

/* Create a frozen copy of l's current buffers, positioned at l's current
 * arranged anchor (the copy root is set to the anchor; the clones inside are
 * relative to it). Placed just above the real bar node within the same layer
 * tree, so it renders in the bar's z-band — below the fixed notch — and the
 * desktops slide behind it. */
static struct wlr_scene_tree *
slide_layer_frozen_create(LayerSurface *l)
{
	struct wlr_scene_tree *frozen, *root_tree;
	struct wlr_scene_node *child;

	if (!l || !l->scene || !l->scene->node.parent)
		return NULL;
	frozen = wlr_scene_tree_create(l->scene->node.parent);
	if (!frozen)
		return NULL;
	wlr_scene_node_set_position(&frozen->node, l->slide_anchor_x, l->slide_anchor_y);
	root_tree = wlr_scene_tree_from_node(&l->scene->node);
	wl_list_for_each(child, &root_tree->children, link)
		slide_layer_frozen_clone(child, frozen, 0, 0);
	wlr_scene_node_place_above(&frozen->node, &l->scene->node);
	return frozen;
}

/* The sliding layer surface died mid-slide (client restarted): detach our
 * listener (the surface's signal list is going away, so it must not be
 * removed again from teardown) and clear the pointer. The frozen copy keeps
 * its own buffer locks and can finish sliding. */
static void
slide_layer_surface_destroy(struct wl_listener *listener, void *data)
{
	struct slide_layer *sl = wl_container_of(listener, sl, surface_destroy);
	(void)data;
	wl_list_remove(&sl->surface_destroy.link);
	wl_list_init(&sl->surface_destroy.link);
	sl->l = NULL;
}

static void
slide_layers_capture(Monitor *m)
{
	int li;
	slide.layers_count = 0;
	for (li = 0; li < 4; li++) {
		LayerSurface *l;
		wl_list_for_each(l, &m->layers[li], link) {
			struct slide_layer *sl;
			if (slide.layers_count >= SLIDE_MAX_LAYERS)
				return;
			if (!l->mapped || !l->scene || !l->layer_surface->namespace)
				continue;
			if (!slide_layer_ns_match(l->layer_surface->namespace))
				continue;
			sl = &slide.layers[slide.layers_count++];
			sl->l = l;
			sl->frozen = slide_layer_frozen_create(l);
			sl->surface_destroy.notify = slide_layer_surface_destroy;
			wl_signal_add(&l->layer_surface->events.destroy, &sl->surface_destroy);
		}
	}
}

/* Move the sliding layer surfaces. The real, live bar node follows the
 * incoming desktop (offset_x like clients) and already shows the arriving
 * workspace; the frozen copy of its outgoing buffers slides out with the
 * outgoing desktop. Anchors come from the LAST arranged anchor (kept fresh by
 * arrangelayer() mid-slide), so a mid-slide re-arrange is tracked instead of
 * snapping back to the slide-start position. */
static void
slide_apply_layers(int off_out, int off_in)
{
	int i;
	for (i = 0; i < slide.layers_count; i++) {
		struct slide_layer *sl = &slide.layers[i];
		LayerSurface *l = sl->l;

		if (l && l->scene) {
			/* Current arranged anchor (updated by arrangelayer() on every
			 * configure, including mid-slide commits). */
			sl->anchor_x = l->slide_anchor_x;
			sl->anchor_y = l->slide_anchor_y;

			/* Incoming desktop: real, live bar glides in. layer_apply_position
			 * composes the reveal offset on top, so revealing mid-slide keeps
			 * both offsets. */
			l->slide_offset_x = off_in;
			layer_apply_position(l);
		}
		/* If the surface died mid-slide, keep sliding the frozen copy from
		 * the last known anchor; the real node is gone. */

		/* Outgoing desktop: frozen copy slides out. */
		if (sl->frozen)
			wlr_scene_node_set_position(&sl->frozen->node,
				sl->anchor_x + off_out, sl->anchor_y);
	}
}

/* Restore the live bar to its arranged anchor (offset 0) and destroy the
 * frozen copy. Safe whether the surface is still alive or was destroyed
 * mid-slide (its destroy listener already removed itself). */
static void
slide_layers_teardown(void)
{
	int i;
	for (i = 0; i < slide.layers_count; i++) {
		struct slide_layer *sl = &slide.layers[i];
		LayerSurface *l = sl->l;

		if (!wl_list_empty(&sl->surface_destroy.link))
			wl_list_remove(&sl->surface_destroy.link);
		if (sl->frozen)
			wlr_scene_node_destroy(&sl->frozen->node); /* releases buffer locks */
		if (l && l->scene) {
			/* Restore the live bar to its arranged anchor plus any reveal
			 * offset still active (the reveal survives the slide; it is the
			 * slide offset that is being dropped). */
			l->slide_offset_x = 0;
			layer_apply_position(l);
		}
	}
	slide.layers_count = 0;
}

/* ========================================================================
 * Slide lifecycle
 * ======================================================================== */

/* Emit the tag::slide_start / tag::slide_end Lua signal on the new tag, with
 * (old_tag, new_tag, direction) as arguments. old_tag may be nil when the
 * outgoing tag was deleted (focus-space leave). luaA_object_emit_signal()
 * pops only the arguments and leaves the object on the stack, so pop exactly
 * it.
 *
 * Never push or emit on a dead tag or a collected Lua object: a tag removed
 * from globalconf.tags (or deactivated) must not receive a signal, and
 * luaA_object_push() of an object whose userdata was collected yields nil,
 * which would make luaA_object_emit_signal() fire on the wrong stack value.
 * Both are checked before anything is pushed. */
static void
emit_slide_signal(const char *name, tag_t *old, tag_t *new, int direction)
{
	lua_State *L = globalconf_get_lua_State();

	if (!L || !new || !tag_is_alive(new))
		return;
	if (old && !tag_is_alive(old))
		old = NULL; /* deleted tag: pass nil, matching a focus-space leave */

	luaA_object_push(L, new);
	if (lua_type(L, -1) != LUA_TUSERDATA) {
		lua_pop(L, 1);
		return; /* Lua object was collected */
	}

	if (old) {
		luaA_object_push(L, old);
		if (lua_type(L, -1) != LUA_TUSERDATA) {
			lua_pop(L, 2);
			return; /* Lua object was collected */
		}
	} else {
		lua_pushnil(L);
	}

	luaA_object_push(L, new);
	lua_pushinteger(L, direction);
	luaA_object_emit_signal(L, -4, name, 3);
	lua_pop(L, 1);
}

static void
emit_slide_end(void)
{
	emit_slide_signal("slide_end", slide.old_tag, slide.new_tag,
		slide.direction);
}

static void
slide_teardown(bool emit_signal)
{
	if (!slide.active)
		return;
	slide.active = false;

	/* Push the outgoing desktop to its final off-screen offset, then ban it:
	 * it was kept enabled for the animation and is no longer wanted. */
	slide_apply_desktop(&slide.out, -slide.direction * slide.distance);
	slide_apply_desktop(&slide.in, 0);
	for (int i = 0; i < slide.out.clients.len; i++) {
		Client *c = slide.out.clients.tab[i];
		if (!c || !c->scene)
			continue;
		c->visual_offset_x = 0;
		wlr_scene_node_set_enabled(&c->scene->node, false);
		c->isbanned = true;
		client_offset_input_clear(c);
		c->slide_input_blocked = false;
	}
	for (int i = 0; i < slide.in.clients.len; i++) {
		Client *c = slide.in.clients.tab[i];
		if (!c || !c->scene)
			continue;
		c->visual_offset_x = 0;
		client_apply_scene_offset(c);
	}

	/* Free the temporary scene nodes. */
	if (slide.out.wallpaper)
		wlr_scene_node_destroy(&slide.out.wallpaper->node);
	if (slide.out.black_bg)
		wlr_scene_node_destroy(&slide.out.black_bg->node);
	if (slide.in.wallpaper)
		wlr_scene_node_destroy(&slide.in.wallpaper->node);
	if (slide.in.black_bg)
		wlr_scene_node_destroy(&slide.in.black_bg->node);
	if (slide.gap_rect)
		wlr_scene_node_destroy(&slide.gap_rect->node);
	for (int i = 0; i < slide.static_bg_count; i++)
		if (slide.static_bg[i])
			wlr_scene_node_destroy(&slide.static_bg[i]->node);

	/* Destroy the frozen bar copies and restore the live bars to their
	 * arranged anchors (offset 0). Safe whether the surface is alive or was
	 * destroyed mid-slide (its destroy listener already removed itself). */
	slide_layers_teardown();

	/* Restore the global scene visibility we took over for the slide:
	 * every wallpaper node back to the exact enabled state it had at slide
	 * start, and the root background. A node destroyed or replaced mid-slide
	 * (wallpaper change) cleared its destroy listener, so it is skipped; a
	 * per-screen cache node that is no longer the current one for its screen
	 * is left hidden rather than resurrected over the new wallpaper. */
	for (int hidx = 0; hidx < slide.hidden_count; hidx++) {
		struct slide_wp_node_hidden *h = &slide.hidden[hidx];
		if (!wl_list_empty(&h->destroy.link))
			wl_list_remove(&h->destroy.link);
		if (!h->node)
			continue;
		if (h->is_legacy) {
			wlr_scene_node_set_enabled(&h->node->node, h->enabled);
		} else if (h->screen_index >= 0 && h->screen_index < WALLPAPER_MAX_SCREENS) {
			wallpaper_cache_entry_t *e = globalconf.current_wallpaper_per_screen[h->screen_index];
			if (e && e->scene_node == h->node)
				wlr_scene_node_set_enabled(&h->node->node, h->enabled);
		}
	}
	slide.hidden_count = 0;
	some_slide_set_root_bg_visible(slide.root_bg_visible);
	/* The fullscreen background must reflect the ARRIVING desktop, not the
	 * pre-slide state: recompute it from the top visible client (the same
	 * decision arrange() makes), so a fullscreen client never leaves a black
	 * rect behind on a non-fullscreen tag. */
	if (slide.mon->fullscreen_bg) {
		Client *top = focustop(slide.mon);
		wlr_scene_node_set_enabled(&slide.mon->fullscreen_bg->node,
			top && top->fullscreen);
	}

	client_array_wipe(&slide.out.clients);
	client_array_wipe(&slide.in.clients);

	/* Re-sync the persistent focus-space backdrop for the arriving tag. */
	slide_backdrop_refresh(slide.mon);

	motionnotify(0, NULL, 0, 0, 0, 0);

	if (emit_signal)
		emit_slide_end();
}

/* Finish a slide and tell Lua. Emitted signals may run arbitrary config code,
 * which is fine on the normal completion path but must never happen while a
 * Lua state is being torn down (hot-reload uses the silent variant). */
static void
slide_finish(void)
{
	slide_teardown(true);
}

/* Read the tag's Lua-side index (tag._private.awful_tag_properties.index),
 * which the C tag_t does not carry. Returns -1 when unavailable. The pushed
 * object can be nil if the tag's Lua object was collected, so never index it
 * without checking the type first. */
static int
tag_lua_index(tag_t *t)
{
	lua_State *L = globalconf_get_lua_State();
	int idx = -1;
	if (!L || !t)
		return -1;
	luaA_object_push(L, t);
	if (lua_type(L, -1) != LUA_TUSERDATA) {
		lua_pop(L, 1);
		return -1;
	}
	lua_getfield(L, -1, "_private");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "awful_tag_properties");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "index");
			if (lua_isnumber(L, -1))
				idx = (int)lua_tointeger(L, -1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 2);
	return idx;
}

static void
slide_start(Monitor *m, tag_t *old, int old_backdrop, int old_index, tag_t *new)
{
	Client *c;
	int i;
	int oi;

	slide_finish();

	if (!m || !m->wlr_output || !m->wlr_output->enabled || m->m.width <= 0)
		return;

	slide.active = true;
	slide.mon = m;
	slide.old_tag = old;
	slide.new_tag = new;
	slide.direction = +1; /* default; refined below */
	slide.distance = m->m.width + slide_gap;
	slide.t0 = clock_now();
	slide.duration = slide_duration;
	slide.easing = slide_easing;
	slide.eased = 0.0;
	slide.manual = false;
	slide.frames = 0;
	slide.last_apply = clock_now();
	slide.last_frame_apply = clock_now();

	client_array_init(&slide.out.clients);
	client_array_init(&slide.in.clients);
	slide.out.wallpaper = NULL;
	slide.out.black_bg = NULL;
	slide.in.wallpaper = NULL;
	slide.in.black_bg = NULL;
	slide.gap_rect = NULL;
	slide.static_bg_count = 0;
	memset(slide.static_bg, 0, sizeof(slide.static_bg));
	slide.hidden_count = 0;
	slide.black_warned = false;

	/* Direction by tag index: a higher index enters from the right. The old
	 * index comes from the snapshot (a deleted focus-space temp tag has no
	 * live object to read it from). */
	oi = old_index;
	if (oi < 0 && old)
		oi = tag_lua_index(old);
	{
		int ni = tag_lua_index(new);
		if (oi >= 0 && ni >= 0)
			slide.direction = (ni > oi) ? +1 : -1;
	}

	/* Collect the sliding clients. Clients visible on BOTH tags (or sticky)
	 * stay put: they are on the arriving desktop already. When `old` is NULL
	 * the outgoing tag was deleted (focus-space leave): its desktop is empty
	 * by construction (a tag with clients cannot be deleted), so there is
	 * nothing to slide out of it. */
	for (i = 0; i < globalconf.clients.len; i++) {
		c = globalconf.clients.tab[i];
		if (!c || !c->scene || c->mon != m)
			continue;
		bool on_old = old && is_client_tagged(c, old);
		bool on_new = is_client_tagged(c, new);
		if (c->sticky || (on_old && on_new))
			continue;
		if (on_old) {
			client_array_append(&slide.out.clients, c);
			c->visual_offset_x = 0;
			wlr_scene_node_set_enabled(&c->scene->node, true);
			c->isbanned = false;
			client_slide_input_apply(c, true);
			client_apply_scene_offset(c);
		} else if (on_new) {
			client_array_append(&slide.in.clients, c);
			c->visual_offset_x = slide.direction * slide.distance;
			wlr_scene_node_set_enabled(&c->scene->node, true);
			c->isbanned = false;
			client_apply_scene_offset(c);
		}
	}

	/* Per-desktop backdrops for this screen. */
	desktop_backdrop_setup(&slide.out, old_backdrop, m, &m->m);
	desktop_backdrop_setup(&slide.in, new->backdrop, m, &m->m);

	/* The gap strip between the two desktops. */
	slide.gap_rect = wlr_scene_rect_create(layers[LyrBg],
		slide_gap, m->m.height, slide_gap_color);
	if (slide.gap_rect)
		wlr_scene_node_set_position(&slide.gap_rect->node,
			m->m.x + m->m.width, m->m.y);

	/* Static per-screen wallpaper copies for every OTHER monitor, so hiding
	 * the shared wallpaper (and the root background) does not black them out
	 * while the slide runs. Same fallback chain as the desktops: slide-owned
	 * snapshot → visible wallpaper node → nothing (and the single warning). */
	{
		Monitor *o;
		wl_list_for_each(o, &mons, link) {
			struct slide_wp_source *osrc;
			struct wlr_scene_buffer *onode;
			if (o == m)
				continue;
			if (slide.static_bg_count >= SLIDE_MAX_STATIC)
				break;
			onode = NULL;
			osrc = slide_wp_ensure(o);
			if (osrc)
				onode = wallpaper_crop_from_source(osrc, &o->m);
			if (!onode && globalconf.wallpaper_buffer_node)
				onode = wallpaper_crop_from_node(globalconf.wallpaper_buffer_node, &o->m);
			if (onode)
				slide.static_bg[slide.static_bg_count++] = onode;
			else if (!slide.black_warned) {
				fprintf(stderr, "somewm: slide: WARNING: no wallpaper "
					"backdrop available for a static monitor; it will "
					"show black during the slide\n");
				slide.black_warned = true;
			}
		}
	}

	/* Capture the sliding layer surfaces (the bar) and pre-position them at
	 * the eased=0 offsets, the same as the clients above: the frozen copy
	 * holds the outgoing desktop's bar in place, the real bar starts off to
	 * the incoming side. */
	slide_layers_capture(m);
	slide_apply_layers(0, slide.direction * slide.distance);

	/* Take over the global scene visibility: record the exact enabled state
	 * of every wallpaper node (legacy + per-screen cache) and the root
	 * background, then hide them so everything revealed around the desktops
	 * is black. Hide the fullscreen background too so a fullscreen client's
	 * black rect cannot cover the incoming desktop sliding in. Teardown
	 * restores each wallpaper node and the root background to exactly the
	 * state recorded here; a node replaced mid-slide (wallpaper change) is
	 * left as the wallpaper system set it. */
	if (globalconf.wallpaper_buffer_node)
		slide_hide_wp_node(globalconf.wallpaper_buffer_node, true, -1);
	for (int si = 0; si < WALLPAPER_MAX_SCREENS; si++) {
		wallpaper_cache_entry_t *e = globalconf.current_wallpaper_per_screen[si];
		if (e && e->scene_node)
			slide_hide_wp_node(e->scene_node, false, si);
	}
	slide.root_bg_visible = some_slide_root_bg_visible();
	some_slide_set_root_bg_visible(false);
	if (m->fullscreen_bg)
		wlr_scene_node_set_enabled(&m->fullscreen_bg->node, false);

	/* Keyboard focus: the new tag's top client takes it at the start. */
	c = focustop(m);
	if (!c || !is_client_tagged(c, new)) {
		c = NULL;
		for (i = 0; i < slide.in.clients.len; i++) {
			if (slide.in.clients.tab[i] && slide.in.clients.tab[i]->scene
					&& slide.in.clients.tab[i]->focusable
					&& !slide.in.clients.tab[i]->nofocus) {
				c = slide.in.clients.tab[i];
				break;
			}
		}
	}
	if (c && c->scene)
		focusclient(c, 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

	emit_slide_signal("slide_start", old, new, slide.direction);

	/* Drive from output frames (vsync); the 1 ms timer is the fallback. */
	wlr_output_schedule_frame(m->wlr_output);
	wl_event_source_timer_update(slide.timer, 1);
}

/* ========================================================================
 * Driver entry points
 * ======================================================================== */

static void slide_tick_impl(bool from_frame);

void
slide_set_progress(double p)
{
	if (!slide.active)
		return;
	slide.manual = true;
	slide.eased = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
	slide_apply(slide.eased);
	if (slide.eased >= 1.0)
		slide_finish();
}

void
slide_tick(void)
{
	slide_tick_impl(false);
}

/* Called from rendermon(): this is the vsync-aligned primary driver. */
void
slide_tick_frame(void)
{
	slide_tick_impl(true);
}

static void
slide_tick_impl(bool from_frame)
{
	double now;
	double p;

	if (!slide.active)
		return;

	if (slide.manual) {
		if (slide.eased >= 1.0) {
			slide_finish();
		}
		return;
	}

	now = clock_now();
	p = (now - slide.t0) / slide.duration;
	if (p < 0.0)
		p = 0.0;
	else if (p > 1.0)
		p = 1.0;

	slide.eased = animation_ease(slide.easing, p);

	if (from_frame) {
		/* Output frame event: the scene apply lands right before the commit,
		 * so the presented frame carries the freshest position. */
		slide.last_frame_apply = now;
		if (now - slide.last_apply >= 1.0 / 120.0) {
			slide.last_apply = now;
			slide_apply(slide.eased);
		}
	} else if (now - slide.last_frame_apply >= 0.05) {
		/* Fallback (1 ms timer / some_refresh): only re-apply when the output
		 * has stalled for 50 ms with no frame event, so the vsync driver
		 * wins whenever frames are flowing. */
		if (now - slide.last_apply >= 1.0 / 120.0) {
			slide.last_apply = now;
			slide_apply(slide.eased);
		}
	}

	if (p >= 1.0)
		slide_finish();
}

bool
slide_active(void)
{
	return slide.active;
}

bool
slide_active_on(Monitor *m)
{
	return slide.active && slide.mon == m;
}

/* ========================================================================
 * Persistent focus-space backdrop (independent of the animation)
 * ======================================================================== */

void
slide_backdrop_refresh(Monitor *m)
{
	tag_t *t;
	bool want_black = false;

	if (!m)
		return;

	/* Any selected tag on this monitor with a black backdrop wins. */
	for (int i = 0; i < globalconf.tags.len; i++) {
		t = globalconf.tags.tab[i];
		if (!t || !t->selected || !t->screen || t->screen->monitor != m)
			continue;
		if (t->backdrop == TAG_BACKDROP_BLACK) {
			want_black = true;
			break;
		}
	}

	if (want_black) {
		if (!m->black_bg) {
			m->black_bg = black_rect_create(&m->m);
			if (!m->black_bg)
				return;
			/* Keep it above the wallpaper node within LyrBg. */
			wlr_scene_node_raise_to_top(&m->black_bg->node);
		}
		wlr_scene_node_set_enabled(&m->black_bg->node, true);
	} else if (m->black_bg) {
		wlr_scene_node_set_enabled(&m->black_bg->node, false);
	}
}

void
slide_backdrop_refresh_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		slide_backdrop_refresh(m);
}

/* ========================================================================
 * Tag switch detection (called from banning_refresh())
 * ======================================================================== */

bool
slide_handle_banning(void)
{
	Monitor *m;
	tag_t *cur;
	tag_t *prev;
	bool took = false;

	/* Keep the persistent focus-space backdrop in sync on every banning
	 * cycle (i.e. every tag switch / retag). */
	slide_backdrop_refresh_all();

	/* A switch (or a selection change) landed mid-slide: snap the running slide
	 * to its end now (slide_finish() bans the interrupted outgoing desktop
	 * directly), then the 1->1 detection below starts the follow-up slide.
	 * Only a real change of the selected tag interrupts: a retag that keeps
	 * the slide's destination selected must not abort a healthy slide.
	 * need_lazy_banning is left untouched here: if no new slide takes over,
	 * the normal banning pass below still has to apply the new selection. */
	if (slide.active) {
		tag_t *still = single_selected_on(slide.mon);
		if (still != slide.new_tag)
			slide_finish();
	}

	if (!globalconf.need_lazy_banning)
		return false;

	wl_list_for_each(m, &mons, link) {
		struct prev_sel *p;
		cur = single_selected_on(m);
		p = prev_selected_get(m);
		prev = p ? p->tag : NULL;
		if (cur && prev && cur != prev && slide_enabled
				&& tag_is_alive(cur)
				&& m->wlr_output && m->wlr_output->enabled) {
			bool old_alive = tag_is_alive(prev);
			int old_backdrop = old_alive ? prev->backdrop : p->backdrop;
			int old_index = p->index;
			prev_selected_set(m, cur);
			/* A deleted outgoing tag (focus-space leave) slides out as an
			 * empty desktop built from the snapshot. */
			slide_start(m, old_alive ? prev : NULL, old_backdrop,
				old_index, cur);
			took = true;
			break;
		}
		if (cur != prev)
			prev_selected_set(m, cur);
	}

	if (took) {
		globalconf.need_lazy_banning = false;
		return true;
	}
	return false;
}

/* ========================================================================
 * Timer fallback + hot reload
 * ======================================================================== */

static int
slide_timer_cb(void *data)
{
	(void)data;
	slide_tick();
	if (slide.active) {
		/* Nudge the output so rendermon() fires and takes over the driving;
		 * the tick above only applies if the output has been stalled. */
		if (slide.mon && slide.mon->wlr_output)
			wlr_output_schedule_frame(slide.mon->wlr_output);
		wl_event_source_timer_update(slide.timer, 1);
	}
	return 0;
}

void
slide_init(void)
{
	client_array_init(&slide.out.clients);
	client_array_init(&slide.in.clients);
	slide.timer = wl_event_loop_add_timer(some_get_event_loop(),
		slide_timer_cb, NULL);
}

void
slide_hot_reload(lua_State *L)
{
	(void)L;
	/* Abort any running slide WITHOUT emitting slide_end: the Lua state is
	 * being torn down, so no signal may run on it. Drop the previous-selection
	 * snapshot so the post-reload first switch (or session restore) is treated
	 * as a fresh baseline: instant, never a slide. */
	if (slide.active)
		slide_teardown(false);
	prev_selected_clear_all();
	/* The wallpaper is about to be rebuilt by the new rc.lua
	 * (request::wallpaper::connected re-applies it); drop the slide-owned
	 * wallpaper snapshots so the next slide rebuilds from the fresh surface. */
	slide_wallpaper_changed();
	/* The namespace list is re-registered by the new rc.lua via
	 * slide.set_sliding_layers(); free the old strings so a config that no
	 * longer opts in cannot leave stale namespaces behind. */
	for (int i = 0; i < slide_layer_ns_count; i++)
		free(slide_layer_ns[i]);
	slide_layer_ns_count = 0;
	if (slide.timer)
		wl_event_source_timer_update(slide.timer, 0);
}

/* ========================================================================
 * Lua API: the `slide` global
 * ======================================================================== */

static int
luaA_slide_set_enabled(lua_State *L)
{
	slide_set_enabled(luaA_checkboolean(L, 1));
	return 0;
}

static int
luaA_slide_get_enabled(lua_State *L)
{
	lua_pushboolean(L, slide_enabled);
	return 1;
}

static int
luaA_slide_set_duration(lua_State *L)
{
	slide_set_duration(luaL_checknumber(L, 1));
	return 0;
}

static int
luaA_slide_get_duration(lua_State *L)
{
	lua_pushnumber(L, slide_duration);
	return 1;
}

static int
luaA_slide_set_easing(lua_State *L)
{
	const char *e = luaL_checkstring(L, 1);
	if (!e || strcmp(e, "linear") == 0)
		slide_easing = EASING_LINEAR;
	else if (strcmp(e, "ease-out-cubic") == 0)
		slide_easing = EASING_EASE_OUT_CUBIC;
	else if (strcmp(e, "ease-in-out-cubic") == 0)
		slide_easing = EASING_EASE_IN_OUT_CUBIC;
	else if (strcmp(e, "spring") == 0
			|| strcmp(e, "spring-critical") == 0
			|| strcmp(e, "macos") == 0)
		slide_easing = EASING_SPRING;
	return 0;
}

static int
luaA_slide_get_easing(lua_State *L)
{
	switch (slide_easing) {
	case EASING_LINEAR:
		lua_pushliteral(L, "linear");
		break;
	case EASING_SPRING:
		lua_pushliteral(L, "spring");
		break;
	case EASING_EASE_IN_OUT_CUBIC:
		lua_pushliteral(L, "ease-in-out-cubic");
		break;
	case EASING_EASE_OUT_CUBIC:
	default:
		lua_pushliteral(L, "ease-out-cubic");
		break;
	}
	return 1;
}

static int
luaA_slide_set_gap(lua_State *L)
{
	slide_set_gap((int)luaL_checkinteger(L, 1));
	return 0;
}

static int
luaA_slide_get_gap(lua_State *L)
{
	lua_pushinteger(L, slide_gap);
	return 1;
}

static int
luaA_slide_set_gap_color(lua_State *L)
{
	float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const char *hex = luaL_checkstring(L, 1);
	unsigned r, g, b;
	if (hex && sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3) {
		rgba[0] = r / 255.0f;
		rgba[1] = g / 255.0f;
		rgba[2] = b / 255.0f;
	}
	slide_set_gap_color(rgba);
	return 0;
}

static int
luaA_slide_get_gap_color(lua_State *L)
{
	char hex[8];
	snprintf(hex, sizeof(hex), "#%02x%02x%02x",
		(int)(slide_gap_color[0] * 255.0f),
		(int)(slide_gap_color[1] * 255.0f),
		(int)(slide_gap_color[2] * 255.0f));
	lua_pushstring(L, hex);
	return 1;
}

static int
luaA_slide_set_progress(lua_State *L)
{
	slide_set_progress(luaL_checknumber(L, 1));
	return 0;
}

static int
luaA_slide_get_active(lua_State *L)
{
	lua_pushboolean(L, slide.active);
	return 1;
}

static int
luaA_slide_get_frames(lua_State *L)
{
	lua_pushinteger(L, slide.frames);
	return 1;
}

static int
luaA_slide_set_sliding_layers(lua_State *L)
{
	const char *names[SLIDE_MAX_LAYER_NS];
	int count = 0;

	if (!lua_istable(L, 1))
		return luaL_argerror(L, 1, "expected a table of layer-shell namespaces");
	lua_pushnil(L);
	while (lua_next(L, 1) != 0 && count < SLIDE_MAX_LAYER_NS) {
		if (lua_type(L, -1) == LUA_TSTRING)
			names[count++] = lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	slide_set_sliding_layers(names, count);
	return 0;
}

static const struct luaL_Reg slide_methods[] = {
	{ "set_enabled", luaA_slide_set_enabled },
	{ "enabled", luaA_slide_get_enabled },
	{ "set_duration", luaA_slide_set_duration },
	{ "duration", luaA_slide_get_duration },
	{ "set_easing", luaA_slide_set_easing },
	{ "easing", luaA_slide_get_easing },
	{ "set_gap", luaA_slide_set_gap },
	{ "gap", luaA_slide_get_gap },
	{ "set_gap_color", luaA_slide_set_gap_color },
	{ "gap_color", luaA_slide_get_gap_color },
	{ "set_progress", luaA_slide_set_progress },
	{ "active", luaA_slide_get_active },
	{ "frames", luaA_slide_get_frames },
	{ "set_sliding_layers", luaA_slide_set_sliding_layers },
	{ NULL, NULL }
};

static const struct luaL_Reg slide_meta[] = {
	{ NULL, NULL }
};

void
slide_setup(lua_State *L)
{
	luaA_openlib(L, "slide", slide_methods, slide_meta);
}