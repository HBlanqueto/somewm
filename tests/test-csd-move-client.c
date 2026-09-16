/**
 * test-csd-move-client - Minimal CSD client that sends request_move on drag.
 *
 * Mimics GTK: creates xdg_toplevel with CLIENT_SIDE decorations, handles
 * pointer events, calls xdg_toplevel.request_move() on drag past threshold.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *g_compositor;
static struct wl_seat *g_seat;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_xdg_wm_base;
static struct zxdg_decoration_manager_v1 *g_deco_mgr;

static struct wl_surface *g_surface;
static struct xdg_surface *g_xdg_surface;
static struct xdg_toplevel *g_toplevel;
static struct zxdg_toplevel_decoration_v1 *g_decoration;
static struct wl_pointer *g_pointer;

static bool g_decorated;
static bool g_button_pressed;
static uint32_t g_press_serial;
static int32_t g_press_sx, g_press_sy;

#define SURFACE_W 400
#define SURFACE_H 300
#define DRAG_THRESHOLD 5

static void create_shm_buffer(void) {
    if (!g_shm || !g_surface) { fprintf(stderr, "BUFFER: no shm or surface\n"); return; }

    int width = SURFACE_W, height = SURFACE_H, stride = width * 4;
    int size = stride * height;

    char tmpl[] = "/tmp/csd-buffer-XXXXXX";
    int tmpfd = mkstemp(tmpl);
    if (tmpfd < 0) { fprintf(stderr, "BUFFER: mkstemp failed\n"); return; }
    unlink(tmpl);
    if (ftruncate(tmpfd, size) < 0) {
        fprintf(stderr, "BUFFER: ftruncate failed\n");
        close(tmpfd);
        return;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, tmpfd, 0);
    if (data == MAP_FAILED) { fprintf(stderr, "BUFFER: mmap failed\n"); close(tmpfd); return; }

    uint32_t *pixels = data;
    for (int i = 0; i < width * height; i++)
        pixels[i] = 0xFF336699;

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, tmpfd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(tmpfd);
    munmap(data, size);

    wl_surface_attach(g_surface, buffer, 0, 0);
    wl_surface_damage_buffer(g_surface, 0, 0, width, height);
    wl_surface_commit(g_surface);
    fprintf(stderr, "BUFFER: attached %dx%d, committed\n", width, height);
}

/* ---- xdg_wm_base ---- */
static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

/* ---- xdg_surface ---- */
static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surf, serial);
    fprintf(stderr, "READY: xdg_surface configured\n");
    create_shm_buffer();
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };

/* ---- xdg_toplevel ---- */
static void toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h,
                                struct wl_array *states) {
    (void)data; (void)tl; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *tl) {
    (void)data; (void)tl;
    fprintf(stderr, "CLOSE\n");
    exit(0);
}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h) {
    (void)data; (void)tl; (void)w; (void)h;
}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl, struct wl_array *caps) {
    (void)data; (void)tl; (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds, .wm_capabilities = toplevel_wm_capabilities,
};

/* ---- xdg-decoration ---- */
static void deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode) {
    (void)data; (void)deco;
    fprintf(stderr, "DECO mode: %s\n",
            mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ? "CLIENT" :
            mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? "SERVER" : "NONE");
    g_decorated = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = { .configure = deco_configure };

/* ---- wl_pointer ---- */
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)pointer; (void)serial; (void)surface; (void)sx; (void)sy;
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface) {
    (void)data; (void)pointer; (void)serial; (void)surface;
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                            wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)pointer; (void)time;
    int32_t x = wl_fixed_to_int(sx);
    int32_t y = wl_fixed_to_int(sy);

    if (g_button_pressed && g_decorated) {
        int32_t dx = x - g_press_sx;
        int32_t dy = y - g_press_sy;
        if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) {
            fprintf(stderr, "SENDING request_move serial=%u seat=%p\n", g_press_serial, g_seat);
            xdg_toplevel_move(g_toplevel, g_seat, g_press_serial);
            g_button_pressed = false;
        }
    }
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                             uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)pointer; (void)time;
    if (button == 0x110) { /* BTN_LEFT */
        fprintf(stderr, "BUTTON button=%u state=%u serial=%u\n", button, state, serial);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            g_press_serial = serial;
            g_button_pressed = true;
        } else {
            g_button_pressed = false;
        }
    }
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                          uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}
static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data; (void)pointer;
}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    (void)data; (void)pointer; (void)source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* ---- wl_seat ---- */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_pointer) {
        g_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer, &pointer_listener, NULL);
        fprintf(stderr, "SEAT: got pointer\n");
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

/* ---- Registry ---- */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                             const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        g_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g_xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_xdg_wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        g_deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
    /* "nodeco" simulates GNOME apps (Nautilus et al.): they draw their own
     * headerbar but never bind zxdg_decoration_manager_v1, so the compositor
     * sees no decoration object at all. */
    bool nodeco = (argc > 1 && strcmp(argv[1], "nodeco") == 0);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "FAIL: no display\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!nodeco && !g_deco_mgr) {
        fprintf(stderr, "SKIP: no deco manager global, running nodeco\n");
        nodeco = true;
    }

    if (!g_compositor || !g_shm || !g_xdg_wm_base) {
        fprintf(stderr, "FAIL: missing globals (compositor=%p shm=%p xdg=%p)\n",
                g_compositor, g_shm, g_xdg_wm_base);
        return 1;
    }

    g_surface = wl_compositor_create_surface(g_compositor);
    g_xdg_surface = xdg_wm_base_get_xdg_surface(g_xdg_wm_base, g_surface);
    xdg_surface_add_listener(g_xdg_surface, &xdg_surface_listener, NULL);

    g_toplevel = xdg_surface_get_toplevel(g_xdg_surface);
    xdg_toplevel_add_listener(g_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(g_toplevel, "CSD Move Test");
    xdg_toplevel_set_app_id(g_toplevel, "csd-move-test");

    if (!nodeco) {
        g_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(g_deco_mgr, g_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(g_decoration, &deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(g_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    } else {
        g_decorated = true;
        fprintf(stderr, "DECO mode: none (simulated GNOME CSD, no manager bound)\n");
    }

    wl_surface_commit(g_surface);
    fprintf(stderr, "CLIENT: surface committed, waiting for configure...\n");

    while (wl_display_dispatch(display) != -1) {
        /* Keep running to be moved/resized */
    }

    return 0;
}
