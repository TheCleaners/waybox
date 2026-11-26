/*
 * A client with a popup grab, for the lock-screen test: a 400x300 window
 * that, when clicked, opens a popup with a grab (as a context menu does).
 * Prints "configured", "popup-shown", "popup-done" (the compositor
 * dismissed it) and "key" for each key it receives. Runs until stdin
 * closes.
 *
 * "reposition" on stdin places the open popup anew (xdg_popup.reposition,
 * as GTK does once it knows a popover's size): 100x500, under the window's
 * bottom edge, allowed to flip or slide up. Prints each configure as
 * "popup-configure X Y W H" and "popup-repositioned TOKEN".
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdarg.h>
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
static struct xdg_surface *xsurface;
static int configured, popped;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
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


/* ---- the popup ---- */
static struct wl_surface *popup_surface;
static void pxs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(popup_surface, make_buffer(100, 60), 0, 0);
	wl_surface_commit(popup_surface);
	say("popup-shown");
}
static const struct xdg_surface_listener pxs_listener = { .configure = pxs_configure };
static struct xdg_popup *popup;
static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	say("popup-configure %d %d %d %d", x, y, w, h);
}
static void popup_done(void *d, struct xdg_popup *p) { say("popup-done"); }
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t token) {
	say("popup-repositioned %u", token);
}
static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure, .popup_done = popup_done,
	.repositioned = popup_repositioned,
};

static void open_popup(uint32_t serial) {
	popup_surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(wm_base, popup_surface);
	xdg_surface_add_listener(pxs, &pxs_listener, NULL);
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(pos, 100, 60);
	xdg_positioner_set_anchor_rect(pos, 50, 50, 1, 1);
	popup = xdg_surface_get_popup(pxs, xsurface, pos);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	xdg_popup_grab(popup, seat, serial);
	wl_surface_commit(popup_surface);
}

/* ---- input ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf,
		wl_fixed_t x, wl_fixed_t y) {}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t b, uint32_t st) {
	if (st == WL_POINTER_BUTTON_STATE_PRESSED && !popped) {
		popped = 1;
		open_popup(serial);
	}
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button,
	.axis = p_axis, .frame = p_frame, .axis_source = p_axis_source,
	.axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf,
		struct wl_array *keys) {}
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

/* ---- xdg ---- */
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
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
				version < 3 ? version : 3);  /* 3: reposition */
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static void command(const char *line) {
	if (strcmp(line, "reposition") == 0 && popup != NULL &&
			xdg_popup_get_version(popup) >= XDG_POPUP_REPOSITION_SINCE_VERSION) {
		struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
		xdg_positioner_set_size(pos, 100, 500);
		xdg_positioner_set_anchor_rect(pos, 0, 299, 100, 1);
		xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM);
		xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM);
		xdg_positioner_set_constraint_adjustment(pos,
				XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
				XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
		xdg_popup_reposition(popup, pos, 7);
		xdg_positioner_destroy(pos);
	}
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
	wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &kb_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xsurface, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xsurface);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "popup-client");
	wl_surface_commit(surface);

	char buf[256];
	size_t len = 0;
	for (;;) {
		wl_display_flush(display);
		struct pollfd fds[2] = {
			{wl_display_get_fd(display), POLLIN, 0},
			{STDIN_FILENO, POLLIN, 0},
		};
		if (poll(fds, 2, -1) < 0)
			return 1;
		if (fds[0].revents & POLLIN && wl_display_dispatch(display) < 0)
			return 1;
		if (fds[0].revents & (POLLHUP | POLLERR))
			return 1;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
			if (n <= 0)
				return 0;
			len += (size_t) n;
			buf[len] = '\0';
			char *nl;
			while ((nl = strchr(buf, '\n')) != NULL) {
				*nl = '\0';
				command(buf);
				size_t used = (size_t) (nl + 1 - buf);
				memmove(buf, nl + 1, len - used + 1);
				len -= used;
			}
		}
	}
}
