/*
 * Touch test client: a 300x200 window (app_id touch-yes or touch-no).
 * "yes" uses the seat's wl_touch and prints "touch-down X Y",
 * "touch-motion X Y", "touch-up"; "no" has no wl_touch, and prints
 * "button pressed|released" for pointer buttons (touch working as a
 * pointer). Both print "configured", and "has-touch" if the seat offers
 * touch.
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

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static int use_touch, configured;
static struct wl_touch *touch;
static struct wl_pointer *pointer;

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static struct wl_buffer *make_buffer(int W, int H, uint32_t color) {
	size_t stride = W * 4, size = stride * H;
	int fd = memfd_create("buf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = color;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void t_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time,
		struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	printf("touch-down %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}
static void t_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
	say("touch-up");
}
static void t_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id,
		wl_fixed_t x, wl_fixed_t y) {
	printf("touch-motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}
static void t_frame(void *d, struct wl_touch *t) {}
static void t_cancel(void *d, struct wl_touch *t) { say("touch-cancel"); }
static const struct wl_touch_listener touch_listener = {
	.down = t_down, .up = t_up, .motion = t_motion, .frame = t_frame, .cancel = t_cancel,
};

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t button, uint32_t state) {
	say(state == WL_POINTER_BUTTON_STATE_PRESSED ? "button pressed" : "button released");
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
	if (caps & WL_SEAT_CAPABILITY_TOUCH)
		say("has-touch");
	if (use_touch && (caps & WL_SEAT_CAPABILITY_TOUCH) && touch == NULL) {
		touch = wl_seat_get_touch(s);
		wl_touch_add_listener(touch, &touch_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	/* Drawn again on each configure, as toolkits do. */
	wl_surface_attach(surface, make_buffer(300, 200, use_touch ? 0xff3060a0 : 0xff30a060), 0, 0);
	wl_surface_commit(surface);
	if (!configured) {
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
	else if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	use_touch = argc > 1 && strcmp(argv[1], "yes") == 0;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base)
		return 1;
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	wl_seat_add_listener(seat, &seat_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, use_touch ? "touch-yes" : "touch-no");
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
