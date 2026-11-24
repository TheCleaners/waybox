/*
 * A text-input-v3 application for the input method integration test: a
 * solid 400x300 xdg toplevel with a text field. When its text input enters,
 * it enables it with a cursor rectangle (50,60 2x20), surrounding text
 * "hello" and a content type, and prints what it gets:
 * "configured", "ti-enter", "ti-leave", "preedit TEXT", "commit TEXT",
 * "delete BEFORE AFTER", "key KEYCODE STATE" (wl_keyboard).
 * Commands on stdin: "close".
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

#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define W 400
#define H 300

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_text_input_manager_v3 *ti_manager;
static struct zwp_text_input_v3 *text_input;
static struct wl_surface *surface;
static int configured;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static struct wl_buffer *make_buffer(void) {
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

/* ---- text input ---- */
static void ti_enter(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
	say("ti-enter");
	zwp_text_input_v3_enable(ti);
	zwp_text_input_v3_set_surrounding_text(ti, "hello", 5, 5);
	zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
			ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
	zwp_text_input_v3_set_cursor_rectangle(ti, 50, 60, 2, 20);
	zwp_text_input_v3_commit(ti);
}
static void ti_leave(void *d, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
	say("ti-leave");
	zwp_text_input_v3_disable(ti);
	zwp_text_input_v3_commit(ti);
}
static void ti_preedit(void *d, struct zwp_text_input_v3 *ti, const char *text,
		int32_t begin, int32_t end) {
	say("preedit %s %d %d", text ? text : "(null)", begin, end);
}
static void ti_commit(void *d, struct zwp_text_input_v3 *ti, const char *text) {
	say("commit %s", text ? text : "(null)");
}
static void ti_delete(void *d, struct zwp_text_input_v3 *ti, uint32_t before, uint32_t after) {
	say("delete %u %u", before, after);
}
static void ti_done(void *d, struct zwp_text_input_v3 *ti, uint32_t serial) {}
static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = ti_enter, .leave = ti_leave, .preedit_string = ti_preedit,
	.commit_string = ti_commit, .delete_surrounding_text = ti_delete, .done = ti_done,
};

/* ---- keyboard ---- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t,
		uint32_t key, uint32_t state) {
	say("key %u %u", key, state);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
		uint32_t lat, uint32_t lock, uint32_t group) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
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
		wl_surface_attach(surface, make_buffer(), 0, 0);
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
	else if (strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0)
		ti_manager = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static void command(const char *line) {
	if (strcmp(line, "close") == 0)
		exit(0);
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !ti_manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(kb, &kb_listener, NULL);
	text_input = zwp_text_input_manager_v3_get_text_input(ti_manager, seat);
	zwp_text_input_v3_add_listener(text_input, &ti_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "text-input-client");
	xdg_toplevel_set_title(tl, "text-input-client");
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
