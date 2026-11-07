// Logs the scroll events a Wayland client receives: per wl_pointer frame the
// axis distance, value120 steps, source, and stop, and running totals at each
// stop. The scroll-replay test feeds it a recorded HarmonyOS gesture.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static int configured;

static struct {
    double dist;
    int v120, source, stop, axis_events;
} frame;
static double total_dist;
static int total_v120, total_frames, total_stops;

static void axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) frame.dist += wl_fixed_to_double(v);
    frame.axis_events++;
}
static void axis_source(void *d, struct wl_pointer *p, uint32_t s) { frame.source = (int)s; }
static void axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) frame.stop = 1;
}
static void axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t n)
{
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) frame.v120 += n * 120;
}
static void axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t n)
{
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) frame.v120 += n;
}
static void pointer_frame(void *d, struct wl_pointer *p)
{
    if (!frame.axis_events && !frame.stop) return;
    total_dist += frame.dist;
    total_v120 += frame.v120;
    total_frames++;
    printf("SCROLL_PROBE frame dist=%.2f v120=%d source=%d stop=%d\n", frame.dist, frame.v120, frame.source,
           frame.stop);
    if (frame.stop) {
        total_stops++;
        printf("SCROLL_PROBE total dist=%.1f v120=%d frames=%d stops=%d\n", total_dist, total_v120,
               total_frames, total_stops);
    }
    fflush(stdout);
    memset(&frame, 0, sizeof(frame));
}
static void enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {}
static void leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {}
static void motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {}
static void relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener pointer_listener = {
    enter, leave, motion, button, axis, pointer_frame, axis_source, axis_stop, axis_discrete, axis_value120,
    relative_direction,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    static int bound;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !bound) {
        bound = 1;
        wl_pointer_add_listener(wl_seat_get_pointer(s), &pointer_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    if (!strcmp(iface, wl_compositor_interface.name)) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, version < 8 ? version : 8);
        wl_seat_add_listener(seat, &seat_listener, NULL);
        printf("SCROLL_PROBE seat version=%u\n", version < 8 ? version : 8);
    }
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener surface_listener = {surface_configure};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "SCROLL_PROBE no wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xs, &surface_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_set_title(tl, "scroll-probe");
    wl_surface_commit(surface);
    while (!configured) wl_display_dispatch(display);

    const int w = 800, h = 600, stride = w * 4, size = stride * h;
    int fd = memfd_create("scroll-probe", MFD_CLOEXEC);
    ftruncate(fd, size);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; i++) pixels[i] = 0xff3a6ea5;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, w, h);
    wl_surface_commit(surface);
    printf("SCROLL_PROBE ready\n");
    fflush(stdout);
    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
