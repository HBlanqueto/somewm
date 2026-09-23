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
#include "objects/screen.h"
#include "somewm_api.h"
#include "somewm_types.h"
#include "scenefx_compat.h"
#include "window.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>

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
/* somewm.c helper that toggles the root background rect (static there) */
extern void some_slide_set_root_bg_visible(bool visible);

/* ========================================================================
 * Configuration (defaults, Lua-settable via the `slide` global)
 * ======================================================================== */

static bool slide_enabled = true;
static double slide_duration = 0.300;
static int slide_easing = EASING_EASE_OUT_CUBIC;
static int slide_gap = 40;
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
 * Previous-selection snapshot (drives the 1->1 detection)
 * ======================================================================== */

struct prev_sel {
	Monitor *m;
	tag_t *tag;
};

#define MAX_PREV_SEL 64
static struct prev_sel prev_sel[MAX_PREV_SEL];
static int prev_sel_count;

static tag_t *
prev_selected_get(Monitor *m)
{
	for (int i = 0; i < prev_sel_count; i++)
		if (prev_sel[i].m == m)
			return prev_sel[i].tag;
	return NULL;
}

static void
prev_selected_set(Monitor *m, tag_t *t)
{
	for (int i = 0; i < prev_sel_count; i++) {
		if (prev_sel[i].m == m) {
			prev_sel[i].tag = t;
			return;
		}
	}
	if (prev_sel_count < MAX_PREV_SEL) {
		prev_sel[prev_sel_count].m = m;
		prev_sel[prev_sel_count].tag = t;
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
	bool wallpaper_visible;
	bool root_bg_visible;
	struct wl_event_source *timer; /* 1 ms fallback driver */
};

static struct slide_state slide;

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

/* A wlr_scene_buffer showing only `box` (layout/buffer coordinates) of the
 * shared wallpaper buffer, so each desktop carries its own copy of the
 * wallpaper for its screen. NULL when there is no wallpaper. */
static struct wlr_scene_buffer *
wallpaper_crop_create(struct wlr_box *box)
{
	if (!globalconf.wallpaper_buffer_node
			|| !globalconf.wallpaper_buffer_node->buffer)
		return NULL;

	struct wlr_buffer *buf = globalconf.wallpaper_buffer_node->buffer;
	/* wlr_scene_buffer_create() locks the buffer; hold our own ref around the
	 * call so the node always owns one, then drop ours. */
	wlr_buffer_lock(buf);
	struct wlr_scene_buffer *node = wlr_scene_buffer_create(layers[LyrBg], buf);
	wlr_buffer_drop(buf);
	if (!node)
		return NULL;

	struct wlr_fbox src = {
		.x = box->x, .y = box->y,
		.width = box->width, .height = box->height,
	};
	wlr_scene_buffer_set_source_box(node, &src);
	wlr_scene_buffer_set_dest_size(node, box->width, box->height);
	wlr_scene_node_set_position(&node->node, box->x, box->y);
	return node;
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

/* Build the backdrop node for one desktop (called from slide_start). */
static void
desktop_backdrop_setup(struct slide_desktop *d, tag_t *tag, struct wlr_box *box)
{
	d->tag = tag;
	d->black = (tag && tag->backdrop == TAG_BACKDROP_BLACK);
	if (d->black) {
		d->black_bg = black_rect_create(box);
	} else {
		d->wallpaper = wallpaper_crop_create(box);
		if (!d->wallpaper)
			d->black_bg = black_rect_create(box); /* fallback: no wallpaper */
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

static void
slide_apply(double eased)
{
	int off_out = -slide.direction * (int)llround(eased * slide.distance);
	int off_in = slide.direction * (int)llround((1.0 - eased) * slide.distance);

	slide_apply_desktop(&slide.out, off_out);
	slide_apply_desktop(&slide.in, off_in);

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
 * Slide lifecycle
 * ======================================================================== */

/* Emit the tag::slide_start / tag::slide_end Lua signal on the new tag, with
 * (old_tag, new_tag, direction) as arguments. */
static void
emit_slide_signal(const char *name, tag_t *old, tag_t *new, int direction)
{
	lua_State *L = globalconf_get_lua_State();
	if (!L || !old || !new)
		return;
	luaA_object_push(L, new);
	luaA_object_push(L, old);
	luaA_object_push(L, new);
	lua_pushinteger(L, direction);
	luaA_object_emit_signal(L, -4, name, 3);
	lua_pop(L, 4);
}

static void
emit_slide_end(void)
{
	emit_slide_signal("slide_end", slide.old_tag, slide.new_tag,
		slide.direction);
}

static void
slide_finish(void)
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

	/* Restore the global scene visibility we took over for the slide. */
	if (globalconf.wallpaper_buffer_node)
		wlr_scene_node_set_enabled(&globalconf.wallpaper_buffer_node->node,
			slide.wallpaper_visible);
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

	emit_slide_end();
}

/* Read the tag's Lua-side index (tag._private.awful_tag_properties.index),
 * which the C tag_t does not carry. Returns -1 when unavailable. */
static int
tag_lua_index(tag_t *t)
{
	lua_State *L = globalconf_get_lua_State();
	int idx = -1;
	if (!L || !t)
		return -1;
	luaA_object_push(L, t);
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
slide_start(Monitor *m, tag_t *old, tag_t *new)
{
	Client *c;
	int i;

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

	client_array_init(&slide.out.clients);
	client_array_init(&slide.in.clients);
	slide.static_bg_count = 0;
	memset(slide.static_bg, 0, sizeof(slide.static_bg));

	/* Direction by tag index: a higher index enters from the right. */
	{
		int oi = tag_lua_index(old);
		int ni = tag_lua_index(new);
		if (oi >= 0 && ni >= 0)
			slide.direction = (ni > oi) ? +1 : -1;
	}

	/* Collect the sliding clients. Clients visible on BOTH tags (or sticky)
	 * stay put: they are on the arriving desktop already. */
	for (i = 0; i < globalconf.clients.len; i++) {
		c = globalconf.clients.tab[i];
		if (!c || !c->scene || c->mon != m)
			continue;
		bool on_old = is_client_tagged(c, old);
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
	desktop_backdrop_setup(&slide.out, old, &m->m);
	desktop_backdrop_setup(&slide.in, new, &m->m);

	/* The gap strip between the two desktops. */
	slide.gap_rect = wlr_scene_rect_create(layers[LyrBg],
		slide_gap, m->m.height, slide_gap_color);
	if (slide.gap_rect)
		wlr_scene_node_set_position(&slide.gap_rect->node,
			m->m.x + m->m.width, m->m.y);

	/* Static per-screen wallpaper copies for every OTHER monitor, so hiding
	 * the shared wallpaper (and the root background) does not black them out
	 * while the slide runs. */
	{
		Monitor *o;
		wl_list_for_each(o, &mons, link) {
			if (o == m)
				continue;
			if (slide.static_bg_count >= SLIDE_MAX_STATIC)
				break;
			slide.static_bg[slide.static_bg_count++] =
				wallpaper_crop_create(&o->m);
		}
	}

	/* Take over the global scene visibility: hide the shared wallpaper and
	 * the root background so everything revealed around the desktops is
	 * black, and hide the fullscreen background so a fullscreen client's
	 * black rect cannot cover the incoming desktop sliding in. */
	slide.wallpaper_visible = globalconf.wallpaper_buffer_node
		&& globalconf.wallpaper_buffer_node->node.enabled;
	if (globalconf.wallpaper_buffer_node)
		wlr_scene_node_set_enabled(&globalconf.wallpaper_buffer_node->node, false);
	slide.root_bg_visible = true;
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
	double p;

	if (!slide.active)
		return;

	if (slide.manual) {
		if (slide.eased >= 1.0) {
			slide_finish();
		}
		return;
	}

	p = (clock_now() - slide.t0) / slide.duration;
	if (p < 0.0)
		p = 0.0;
	else if (p > 1.0)
		p = 1.0;

	slide.eased = animation_ease(slide.easing, p);
	slide_apply(slide.eased);

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
		cur = single_selected_on(m);
		prev = prev_selected_get(m);
		if (cur && prev && cur != prev && slide_enabled
				&& m->wlr_output && m->wlr_output->enabled) {
			prev_selected_set(m, cur);
			slide_start(m, prev, cur);
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
	if (slide.active)
		wl_event_source_timer_update(slide.timer, 1);
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
	/* Abort any running slide and drop the previous-selection snapshot, so
	 * the post-reload first switch (or session restore) is treated as a
	 * fresh baseline: instant, never a slide. */
	if (slide.active)
		slide_finish();
	prev_selected_clear_all();
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
	return 0;
}

static int
luaA_slide_get_easing(lua_State *L)
{
	switch (slide_easing) {
	case EASING_LINEAR:
		lua_pushliteral(L, "linear");
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