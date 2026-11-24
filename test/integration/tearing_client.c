/*
 * A game-like client for the tearing test: a fullscreen toplevel that
 * redraws every frame and sets a tearing-control presentation hint.
 * Starts with "async"; stdin "vsync" / "async" change it, "windowed" /
 * "fullscreen" change the window. Prints "configured".
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
#include "tearing-control-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_tearing_control_manager_v1 *tearing_manager;
static struct wl_surface *surface;
static struct xdg_toplevel *toplevel;
static struct wp_tearing_control_v1 *tearing;
static int width = 640, height = 480, configured;

static struct wl_buffer *make_buffer(int W, int H) {
	size_t stride = W * 4, size = stride * H;
	int fd = memfd_create("buf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xff4060a0;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}



static void draw(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
	wl_callback_destroy(cb);
	draw();
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };
static void draw(void) {
	struct wl_callback *cb = wl_surface_frame(surface);
	wl_callback_add_listener(cb, &frame_listener, NULL);
	/* One buffer per size, reused every frame (shm: fine for a test). */
	static struct wl_buffer *buf;
	static int bw, bh;
	if (buf == NULL || bw != width || bh != height) {
		if (buf != NULL)
			wl_buffer_destroy(buf);
		buf = make_buffer(width, height);
		bw = width;
		bh = height;
	}
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		configured = 1;
		draw();
		printf("configured\n");
		fflush(stdout);
	}
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *s) {
	if (w > 0 && h > 0) {
		width = w;
		height = h;
	}
}
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
	else if (strcmp(iface, wp_tearing_control_manager_v1_interface.name) == 0)
		tearing_manager = wl_registry_bind(r, name, &wp_tearing_control_manager_v1_interface, 1);
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
	if (!compositor || !shm || !wm_base || !tearing_manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	tearing = wp_tearing_control_manager_v1_get_tearing_control(tearing_manager, surface);
	wp_tearing_control_v1_set_presentation_hint(tearing,
			WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &tl_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "tearing-client");
	xdg_toplevel_set_fullscreen(toplevel, NULL);
	wl_surface_commit(surface);

	struct pollfd fds[2] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = STDIN_FILENO, .events = POLLIN },
	};
	char line[64];
	while (1) {
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0)
			break;
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(display) < 0)
				break;
		}
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			if (!fgets(line, sizeof(line), stdin))
				break;
			if (strncmp(line, "vsync", 5) == 0)
				wp_tearing_control_v1_set_presentation_hint(tearing,
						WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);
			else if (strncmp(line, "async", 5) == 0)
				wp_tearing_control_v1_set_presentation_hint(tearing,
						WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
			else if (strncmp(line, "windowed", 8) == 0)
				xdg_toplevel_unset_fullscreen(toplevel);
			else if (strncmp(line, "fullscreen", 10) == 0)
				xdg_toplevel_set_fullscreen(toplevel, NULL);
			wl_surface_commit(surface);
		}
	}
	return 0;
}
