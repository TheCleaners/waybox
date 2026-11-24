/*
 * A minimal input method (input-method-v2) for the input method integration
 * test. Prints what Waybox sends it:
 * "ready", "unavailable", "activate", "deactivate", "surrounding TEXT C A",
 * "content HINT PURPOSE", "done", "keymap" (its keyboard grab's keymap),
 * "gkey KEYCODE STATE" (from the grab), "rect X Y W H" (its popup's text
 * cursor rectangle). Like ibus, it makes its virtual keyboard at startup and
 * gives it the grab's keymap.
 * Commands on stdin:
 *   preedit TEXT | commit TEXT | delete BEFORE AFTER
 *   grab            grab the keyboard
 *   forward         from now on, pass grabbed keys on through a virtual
 *                   keyboard (as fcitx5 does with the keys it doesn't use)
 *   popup           show a 200x100 red candidate popup
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
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define PW 200
#define PH 100

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *im_manager;
static struct zwp_virtual_keyboard_manager_v1 *vk_manager;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *grab;
static struct zwp_virtual_keyboard_v1 *vk;
static uint32_t serial;  /* done events received */
static int forward;

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
	size_t stride = PW * 4, size = stride * PH;
	int fd = memfd_create("popup", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xffff0000;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, PW, PH,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

/* ---- input method ---- */
static void im_activate(void *d, struct zwp_input_method_v2 *m) { say("activate"); }
static void im_deactivate(void *d, struct zwp_input_method_v2 *m) { say("deactivate"); }
static void im_surrounding(void *d, struct zwp_input_method_v2 *m, const char *text,
		uint32_t cursor, uint32_t anchor) {
	say("surrounding %s %u %u", text, cursor, anchor);
}
static void im_cause(void *d, struct zwp_input_method_v2 *m, uint32_t cause) {}
static void im_content(void *d, struct zwp_input_method_v2 *m, uint32_t hint,
		uint32_t purpose) {
	say("content %u %u", hint, purpose);
}
static void im_done(void *d, struct zwp_input_method_v2 *m) {
	serial++;
	say("done");
}
static void im_unavailable(void *d, struct zwp_input_method_v2 *m) {
	say("unavailable");
	exit(0);
}
static const struct zwp_input_method_v2_listener im_listener = {
	.activate = im_activate, .deactivate = im_deactivate,
	.surrounding_text = im_surrounding, .text_change_cause = im_cause,
	.content_type = im_content, .done = im_done, .unavailable = im_unavailable,
};

/* ---- keyboard grab ---- */
static void g_keymap(void *d, struct zwp_input_method_keyboard_grab_v2 *g,
		uint32_t format, int32_t fd, uint32_t size) {
	say("keymap");
	zwp_virtual_keyboard_v1_keymap(vk, format, fd, size);
	close(fd);
}
static void g_key(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t s,
		uint32_t time, uint32_t key, uint32_t state) {
	say("gkey %u %u", key, state);
	if (forward && vk != NULL)
		zwp_virtual_keyboard_v1_key(vk, time, key, state);
}
static void g_mods(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t s,
		uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
	if (forward && vk != NULL)
		zwp_virtual_keyboard_v1_modifiers(vk, dep, lat, lock, group);
}
static void g_repeat(void *d, struct zwp_input_method_keyboard_grab_v2 *g,
		int32_t rate, int32_t delay) {}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	.keymap = g_keymap, .key = g_key, .modifiers = g_mods, .repeat_info = g_repeat,
};

/* ---- popup ---- */
static void p_rect(void *d, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	say("rect %d %d %d %d", x, y, w, h);
}
static const struct zwp_input_popup_surface_v2_listener popup_listener = {
	.text_input_rectangle = p_rect,
};

/* A default keymap for the virtual keyboard until the grab sends one. */
static void set_default_keymap(void) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t size = strlen(text) + 1;
	int fd = memfd_create("keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t) size)
		exit(1);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t) size);
	close(fd);
	free(text);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
	else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
		im_manager = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
	else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
		vk_manager = wl_registry_bind(r, name,
				&zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static void command(const char *line) {
	if (strncmp(line, "preedit ", 8) == 0) {
		int len = (int) strlen(line + 8);
		zwp_input_method_v2_set_preedit_string(im, line + 8, len, len);
		zwp_input_method_v2_commit(im, serial);
	} else if (strncmp(line, "commit ", 7) == 0) {
		zwp_input_method_v2_commit_string(im, line + 7);
		zwp_input_method_v2_commit(im, serial);
	} else if (strncmp(line, "delete ", 7) == 0) {
		unsigned before = 0, after = 0;
		sscanf(line + 7, "%u %u", &before, &after);
		zwp_input_method_v2_delete_surrounding_text(im, before, after);
		zwp_input_method_v2_commit(im, serial);
	} else if (strcmp(line, "grab") == 0 && grab == NULL) {
		grab = zwp_input_method_v2_grab_keyboard(im);
		zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
		say("grabbed");
	} else if (strcmp(line, "forward") == 0) {
		forward = 1;
		say("forwarding");
	} else if (strcmp(line, "popup") == 0) {
		struct wl_surface *s = wl_compositor_create_surface(compositor);
		struct zwp_input_popup_surface_v2 *p =
			zwp_input_method_v2_get_input_popup_surface(im, s);
		zwp_input_popup_surface_v2_add_listener(p, &popup_listener, NULL);
		wl_surface_attach(s, make_buffer(), 0, 0);
		wl_surface_commit(s);
		say("popup");
	}
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !im_manager || !vk_manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_manager, seat);
	set_default_keymap();
	im = zwp_input_method_manager_v2_get_input_method(im_manager, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	wl_display_roundtrip(display);
	say("ready");

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
