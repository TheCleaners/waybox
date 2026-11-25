/*
 * pointer-warp and alpha-modifier test client.
 *
 *   warp_alpha_client alpha MULTIPLIER   a 300x200 red window (app_id
 *       "alpha-test") at that opacity (0..1, alpha-modifier-v1)
 *   warp_alpha_client warp               a 300x200 window (app_id
 *       "warp-test") that prints "enter X Y" and "motion X Y"; commands on
 *       stdin: "warp X Y" (with its enter serial), "warp-badserial X Y"
 *
 * Both print "configured" once shown; commands answer "ok".
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wp_alpha_modifier_v1 *alpha_manager;
static struct wp_pointer_warp_v1 *warp;
static struct wl_surface *surface;
static struct wl_pointer *pointer;
static uint32_t enter_serial;
static int configured;

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static struct wl_buffer *make_buffer(int W, int H, uint32_t color) {
	size_t stride = (size_t) W * 4, size = stride * (size_t) H;
	char name[] = "/tmp/wb-warp-buf-XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0)
		exit(1);
	unlink(name);
	if (ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		exit(1);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = color;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, (int32_t) stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
	enter_serial = serial;
	printf("enter %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	say("leave");
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	printf("motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t button, uint32_t state) {}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static uint32_t color = 0xffff0000;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(surface, make_buffer(300, 200, color), 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, 300, 200);
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
	else if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0)
		alpha_manager = wl_registry_bind(r, name, &wp_alpha_modifier_v1_interface, 1);
	else if (strcmp(iface, wp_pointer_warp_v1_interface.name) == 0)
		warp = wl_registry_bind(r, name, &wp_pointer_warp_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s alpha MULTIPLIER | warp\n", argv[0]);
		return 2;
	}
	int alpha_mode = strcmp(argv[1], "alpha") == 0;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base)
		return 1;
	if (alpha_mode && alpha_manager == NULL) {
		say("no-alpha-modifier");
		return 1;
	}
	if (!alpha_mode && warp == NULL) {
		say("no-pointer-warp");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	if (alpha_mode) {
		double m = argc > 2 ? atof(argv[2]) : 1.0;
		struct wp_alpha_modifier_surface_v1 *a =
				wp_alpha_modifier_v1_get_surface(alpha_manager, surface);
		wp_alpha_modifier_surface_v1_set_multiplier(a, (uint32_t) (m * UINT32_MAX));
	} else {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		color = 0xff2060c0;
	}
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, alpha_mode ? "alpha-test" : "warp-test");
	wl_surface_commit(surface);

	struct pollfd fds[2] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = STDIN_FILENO, .events = POLLIN },
	};
	char line[128];
	for (;;) {
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0)
			break;
		if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) && wl_display_dispatch(display) < 0)
			return 1;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			if (fgets(line, sizeof line, stdin) == NULL)
				break;
			int x, y;
			if (sscanf(line, "warp-badserial %d %d", &x, &y) == 2)
				wp_pointer_warp_v1_warp_pointer(warp, surface, pointer,
						wl_fixed_from_int(x), wl_fixed_from_int(y), enter_serial + 1000);
			else if (sscanf(line, "warp %d %d", &x, &y) == 2)
				wp_pointer_warp_v1_warp_pointer(warp, surface, pointer,
						wl_fixed_from_int(x), wl_fixed_from_int(y), enter_serial);
			wl_display_roundtrip(display);
			say("ok");
		}
	}
	return 0;
}
