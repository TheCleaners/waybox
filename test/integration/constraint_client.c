/*
 * Minimal pointer-constraints / relative-pointer client for the pointer
 * lock integration test: a solid 400x300 xdg toplevel that locks or confines
 * the pointer on request and reports what it receives.
 *
 * Prints: "configured", "enter X Y", "motion X Y" (surface-local),
 * "rel DX DY" (relative-pointer motion), "locked"/"unlocked",
 * "confined"/"unconfined".
 * Commands on stdin: "lock", "confine", "release".
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

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define W 400
#define H 300

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct wl_surface *surface;
static struct wl_pointer *pointer;
static struct zwp_locked_pointer_v1 *locked;
static struct zwp_confined_pointer_v1 *confined;
static int configured;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
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

/* ---- constraints ---- */
static void locked_locked(void *d, struct zwp_locked_pointer_v1 *l) { say("locked"); }
static void locked_unlocked(void *d, struct zwp_locked_pointer_v1 *l) { say("unlocked"); }
static const struct zwp_locked_pointer_v1_listener locked_listener = {
	.locked = locked_locked, .unlocked = locked_unlocked,
};
static void confined_confined(void *d, struct zwp_confined_pointer_v1 *c) { say("confined"); }
static void confined_unconfined(void *d, struct zwp_confined_pointer_v1 *c) { say("unconfined"); }
static const struct zwp_confined_pointer_v1_listener confined_listener = {
	.confined = confined_confined, .unconfined = confined_unconfined,
};

/* ---- relative pointer ---- */
static void rel_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi,
		uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy) {
	say("rel %.0f %.0f", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static const struct zwp_relative_pointer_v1_listener rel_listener = {
	.relative_motion = rel_motion,
};

/* ---- pointer ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	say("enter %.0f %.0f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	say("leave");
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	say("motion %.0f %.0f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t b, uint32_t st) {}
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
	else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0)
		constraints = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
	else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0)
		relative_manager = wl_registry_bind(r, name,
				&zwp_relative_pointer_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static void command(const char *line) {
	if (strcmp(line, "lock") == 0 && !locked) {
		locked = zwp_pointer_constraints_v1_lock_pointer(constraints, surface,
				pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(locked, &locked_listener, NULL);
	} else if (strcmp(line, "confine") == 0 && !confined) {
		confined = zwp_pointer_constraints_v1_confine_pointer(constraints, surface,
				pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_confined_pointer_v1_add_listener(confined, &confined_listener, NULL);
	} else if (strcmp(line, "release") == 0) {
		if (locked)
			zwp_locked_pointer_v1_destroy(locked);
		if (confined)
			zwp_confined_pointer_v1_destroy(confined);
		locked = NULL;
		confined = NULL;
		say("released");
	}
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !constraints || !relative_manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	struct zwp_relative_pointer_v1 *rel =
		zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
	zwp_relative_pointer_v1_add_listener(rel, &rel_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "constraint-client");
	xdg_toplevel_set_title(tl, "constraint-client");
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
