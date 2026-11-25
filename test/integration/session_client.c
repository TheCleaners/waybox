/*
 * Session restore test client (xdg-session-management-v1).
 *
 *   session_client APP_ID SESSION_ID|- launch|recover|restore add|restore|late NAME
 *
 * Gets the session (a new one with "-"), then adds its window to it, or asks
 * for it to be restored ("late": after the first commit, a protocol error).
 * Draws at the size it's configured to (300x200 if left to it). Prints:
 *   created ID | session-restored | replaced
 *   toplevel-restored
 *   configured W H        (each configure; the first after "toplevel-restored")
 *   error                 (the connection ended with a protocol error)
 * Commands on stdin: "remove" (forget the session), "destroy" (stop updating
 * it), "remove-toplevel", "quit".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-session-management-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct xdg_session_manager_v1 *manager;
static struct wl_surface *surface;
static struct xdg_session_v1 *session;
static int pending_w, pending_h;
static const char *window_name;

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static struct wl_buffer *make_buffer(int W, int H) {
	size_t stride = (size_t) W * 4, size = stride * (size_t) H;
	char name[] = "/tmp/wb-session-buf-XXXXXX";
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
		px[i] = 0xff4070a0;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, (int32_t) stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	int w = pending_w > 0 ? pending_w : 300, h = pending_h > 0 ? pending_h : 200;
	wl_surface_attach(surface, make_buffer(w, h), 0, 0);
	wl_surface_commit(surface);
	printf("configured %d %d\n", w, h);
	fflush(stdout);
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {
	pending_w = w;
	pending_h = h;
}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tl_listener = {
	.configure = tl_configure, .close = tl_close,
};

static void s_created(void *d, struct xdg_session_v1 *s, const char *id) {
	printf("created %s\n", id);
	fflush(stdout);
}
static void s_restored(void *d, struct xdg_session_v1 *s) { say("session-restored"); }
static void s_replaced(void *d, struct xdg_session_v1 *s) { say("replaced"); }
static const struct xdg_session_v1_listener session_listener = {
	.created = s_created, .restored = s_restored, .replaced = s_replaced,
};

static void ts_restored(void *d, struct xdg_toplevel_session_v1 *t) { say("toplevel-restored"); }
static const struct xdg_toplevel_session_v1_listener toplevel_session_listener = {
	.restored = ts_restored,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, xdg_session_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &xdg_session_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	if (argc != 6) {
		fprintf(stderr, "usage: %s APP_ID SESSION_ID|- launch|recover|restore "
				"add|restore|late NAME\n", argv[0]);
		return 2;
	}
	const char *app_id = argv[1], *mode = argv[4];
	window_name = argv[5];
	uint32_t reason = strcmp(argv[3], "recover") == 0 ? XDG_SESSION_MANAGER_V1_REASON_RECOVER
			: strcmp(argv[3], "restore") == 0 ? XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE
			: XDG_SESSION_MANAGER_V1_REASON_LAUNCH;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !manager) {
		say("no-session-manager");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	session = xdg_session_manager_v1_get_session(manager, reason,
			strcmp(argv[2], "-") == 0 ? NULL : argv[2]);
	xdg_session_v1_add_listener(session, &session_listener, NULL);
	wl_display_roundtrip(display);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, app_id);
	struct xdg_toplevel_session_v1 *ts = NULL;
	if (strcmp(mode, "add") == 0)
		ts = xdg_session_v1_add_toplevel(session, tl, window_name);
	else if (strcmp(mode, "restore") == 0)
		ts = xdg_session_v1_restore_toplevel(session, tl, window_name);
	wl_surface_commit(surface);
	if (strcmp(mode, "late") == 0) {
		wl_display_roundtrip(display);
		ts = xdg_session_v1_restore_toplevel(session, tl, window_name);
	}
	if (ts != NULL)
		xdg_toplevel_session_v1_add_listener(ts, &toplevel_session_listener, NULL);

	struct pollfd fds[2] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = STDIN_FILENO, .events = POLLIN },
	};
	char line[256];
	for (;;) {
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0)
			break;
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (wl_display_dispatch(display) < 0) {
				if (wl_display_get_error(display) == EPROTO)
					say("error");
				return 1;
			}
		}
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			if (fgets(line, sizeof line, stdin) == NULL)
				break;
			if (strncmp(line, "remove-toplevel", 15) == 0) {
				xdg_session_v1_remove_toplevel(session, window_name);
			} else if (strncmp(line, "remove", 6) == 0) {
				xdg_session_v1_remove(session);
				session = NULL;
			} else if (strncmp(line, "destroy", 7) == 0) {
				xdg_session_v1_destroy(session);
				session = NULL;
			} else if (strncmp(line, "quit", 4) == 0) {
				break;
			}
			wl_display_roundtrip(display);
			say("ok");
		}
	}
	wl_display_disconnect(display);
	return 0;
}
