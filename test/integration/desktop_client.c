/*
 * A stand-in for a desktop-icon program (pcmanfm-qt's desktop, xfdesktop):
 * a layer-shell surface covering the output in the given layer
 * (background, bottom or top), that prints the pointer buttons it gets:
 * "button pressed BTN" / "button released BTN". Prints "configured" once
 * shown. Runs until killed.
 *
 *   desktop_client background|bottom|top
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static int configured;

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static struct wl_buffer *make_buffer(int W, int H) {
	size_t stride = (size_t) W * 4, size = stride * (size_t) H;
	char name[] = "/tmp/wb-desktop-buf-XXXXXX";
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
		px[i] = 0xff305070;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, (int32_t) stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t button, uint32_t state) {
	printf("button %s %u\n", state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released",
			button);
	fflush(stdout);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
	say("axis");
}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
	static struct wl_pointer *pointer;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps, .name = seat_name,
};

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
		uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	wl_surface_attach(surface, make_buffer((int) w, (int) h), 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, (int32_t) w, (int32_t) h);
	wl_surface_commit(surface);
	if (!configured) {
		configured = 1;
		say("configured");
	}
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) { exit(0); }
static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = ls_configure, .closed = ls_closed,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 2);
	else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	const char *which = argc > 1 ? argv[1] : "background";
	uint32_t layer = strcmp(which, "bottom") == 0 ? ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM
			: strcmp(which, "top") == 0 ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
			: ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !layer_shell)
		return 1;
	wl_seat_add_listener(seat, &seat_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell, surface, NULL, layer, "desktop");
	zwlr_layer_surface_v1_add_listener(ls, &ls_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ls, 0, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
