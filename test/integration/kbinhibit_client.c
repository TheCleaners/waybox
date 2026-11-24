/*
 * A VM-viewer-like client for the shortcuts-inhibit test: a 400x300 window
 * that asks to take the keyboard shortcuts once it has the keyboard.
 * Prints "configured", "active" / "inactive" (the inhibitor's state) and
 * "key" for each key it receives.
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
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager;
static struct wl_surface *surface;
static struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor;
static int configured;

static void say(const char *s) {
	printf("%s\n", s);
	fflush(stdout);
}

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



static void inh_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { say("active"); }
static void inh_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { say("inactive"); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inh_listener = {
	.active = inh_active, .inactive = inh_inactive,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf,
		struct wl_array *keys) {
	if (inhibitor == NULL) {
		inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
				manager, surface, seat);
		zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &inh_listener, NULL);
	}
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key,
		uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		say("key");
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b,
		uint32_t c, uint32_t g) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {}
static const struct wl_keyboard_listener kb_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
	.modifiers = kb_mods, .repeat_info = kb_repeat,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		configured = 1;
		wl_surface_attach(surface, make_buffer(400, 300), 0, 0);
		wl_surface_commit(surface);
		say("configured");
	}
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *s) {}
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
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name,
				&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
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
	if (!compositor || !shm || !seat || !wm_base || !manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &kb_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "inhibit-client");
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
