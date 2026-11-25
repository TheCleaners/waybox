/*
 * Fractional-scale test client: a window that prints, in order,
 * "preferred N" (wp_fractional_scale_v1, N = scale * 120), "configure"
 * (its first xdg configure), "enter" for each wl_surface.enter, and
 * "configured" once shown. Runs until killed.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_fractional_scale_manager_v1 *fsm;
static struct wl_surface *surface;
static int configured;

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static struct wl_buffer *make_buffer(int W, int H) {
	size_t stride = W * 4, size = stride * H;
	int fd = memfd_create("buf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void preferred(void *d, struct wp_fractional_scale_v1 *f, uint32_t scale) {
	printf("preferred %u\n", scale);
	fflush(stdout);
}
static const struct wp_fractional_scale_v1_listener fs_listener = { .preferred_scale = preferred };
static void s_enter(void *d, struct wl_surface *s, struct wl_output *o) { say("enter"); }
static void s_leave(void *d, struct wl_surface *s, struct wl_output *o) { say("leave"); }
static const struct wl_surface_listener surface_listener = { .enter = s_enter, .leave = s_leave };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		say("configure");
		wl_surface_attach(surface, make_buffer(200, 100), 0, 0);
		wl_surface_commit(surface);
		configured = 1;
		say("configured");
	}
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tl_listener = {
	.configure = tl_configure, .close = tl_close,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0)
		fsm = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
	else if (strcmp(iface, wl_output_interface.name) == 0)
		wl_registry_bind(r, name, &wl_output_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !fsm)
		return 1;
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);
	struct wp_fractional_scale_v1 *fs = wp_fractional_scale_manager_v1_get_fractional_scale(fsm, surface);
	wp_fractional_scale_v1_add_listener(fs, &fs_listener, NULL);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "scale-test");
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
