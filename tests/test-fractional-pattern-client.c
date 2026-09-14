/**
 * XDG shell wl_shm client that mimics fractional-scale rendering.
 *
 * Commits a buffer rendered at 1.5x the logical size with buffer scale 1
 * and presents it via wp_viewport.set_destination(logical_w, logical_h) —
 * the same buffer layout a fractional-scale client (e.g. Firefox on a
 * fractional-scaled output) produces. The compositor must crop the rounded
 * corners against the *displayed* box, not against buffer/scale.
 *
 * Pattern (split by physical buffer dims):
 *   TL = red (0xFFFF0000)  TR = green (0xFF00FF00)
 *   BL = blue (0xFF0000FF)  BR = yellow (0xFFFFFF00)
 *
 * Lifecycle: SIGTERM/SIGINT. App ID: "content_pattern_fractional".
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"

static struct wl_display *g_display;
static struct wl_registry *g_registry;
static struct wl_compositor *g_compositor;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_xdg_wm_base;
static struct wp_viewporter *g_viewporter;

static struct wl_surface *g_surface;
static struct xdg_surface *g_xdg_surface;
static struct xdg_toplevel *g_toplevel;
static struct wp_viewport *g_viewport;
static struct wl_buffer *g_current_buffer;

static int g_logical_w = 200, g_logical_h = 200;
static char g_marker_path[256];
static volatile sig_atomic_t g_running = 1;

static void handle_term(int sig) {
    (void)sig;
    g_running = 0;
}

#define FRAC_NUM 3
#define FRAC_DEN 2   /* 1.5x buffer */

static struct wl_buffer *create_pattern_buffer(int buf_w, int buf_h) {
    if (buf_w <= 0 || buf_h <= 0)
        return NULL;

    int stride = buf_w * 4;
    size_t size = (size_t)stride * (size_t)buf_h;

    char tmpl[] = "/tmp/test-frac-pattern-buf-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        return NULL;
    }
    unlink(tmpl);

    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    /* Full-bleed pattern: the content fills the whole over-scaled buffer, so
     * the displayed corners really are content and the crop is observable. */
    int half_w = buf_w / 2;
    int half_h = buf_h / 2;
    for (int y = 0; y < buf_h; y++) {
        for (int x = 0; x < buf_w; x++) {
            uint32_t color;
            if      (x <  half_w && y <  half_h) color = 0xFFFF0000;
            else if (x >= half_w && y <  half_h) color = 0xFF00FF00;
            else if (x <  half_w && y >= half_h) color = 0xFF0000FF;
            else                                 color = 0xFFFFFF00;
            data[y * buf_w + x] = color;
        }
    }
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, buf_w, buf_h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    return buf;
}

static void render_pattern(void) {
    int buf_w = (g_logical_w * FRAC_NUM) / FRAC_DEN;
    int buf_h = (g_logical_h * FRAC_NUM) / FRAC_DEN;

    struct wl_buffer *buf = create_pattern_buffer(buf_w, buf_h);
    if (!buf) {
        fprintf(stderr, "[frac-pattern-client] buffer alloc failed (%dx%d)\n",
                buf_w, buf_h);
        return;
    }

    /* Buffer scale 1 + viewport destination == fractional presentation. */
    wl_surface_set_buffer_scale(g_surface, 1);
    wl_surface_attach(g_surface, buf, 0, 0);
    wp_viewport_set_destination(g_viewport, g_logical_w, g_logical_h);
    wl_surface_damage_buffer(g_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(g_surface);
    wl_display_flush(g_display);

    g_current_buffer = buf;

    FILE *f = fopen(g_marker_path, "w");
    if (f) {
        fprintf(f, "%dx%d\n", g_logical_w, g_logical_h);
        fclose(f);
    }

    fprintf(stderr, "[frac-pattern-client] committed buf=%dx%d logical=%dx%d\n",
            buf_w, buf_h, g_logical_w, g_logical_h);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    render_pattern();
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)t; (void)states;
    if (w > 0) g_logical_w = w;
    if (h > 0) g_logical_h = h;
}

static void toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)data; (void)t;
    g_running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        g_xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        g_viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char **argv) {
    signal(SIGTERM, handle_term);
    signal(SIGINT, handle_term);

    g_display = wl_display_connect(NULL);
    if (!g_display) {
        fprintf(stderr, "[frac-pattern-client] display connect failed\n");
        return 1;
    }

    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_display);

    if (!g_compositor || !g_shm || !g_xdg_wm_base || !g_viewporter) {
        fprintf(stderr, "[frac-pattern-client] missing globals\n");
        return 1;
    }

    snprintf(g_marker_path, sizeof(g_marker_path),
             "/tmp/test-frac-pattern-%d.marker", getpid());

    g_surface = wl_compositor_create_surface(g_compositor);
    g_xdg_surface = xdg_wm_base_get_xdg_surface(g_xdg_wm_base, g_surface);
    xdg_surface_add_listener(g_xdg_surface, &xdg_surface_listener, NULL);
    g_toplevel = xdg_surface_get_toplevel(g_xdg_surface);
    xdg_toplevel_add_listener(g_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(g_toplevel, "content_pattern_fractional");
    g_viewport = wp_viewporter_get_viewport(g_viewporter, g_surface);

    wl_surface_commit(g_surface);
    wl_display_flush(g_display);

    while (g_running && wl_display_dispatch(g_display) != -1)
        ;

    return 0;
}