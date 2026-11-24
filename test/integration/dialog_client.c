/*
 * A dialog client for the dialogs integration test.
 *
 *   dialog_client export APP_ID
 *       A 400x300 blue window, exported with xdg-foreign v2; prints
 *       "handle HANDLE".
 *   dialog_client import APP_ID HANDLE modal|plain
 *       A 200x100 red window whose parent is the exported window (imported
 *       before the first commit, as a portal does); with "modal", marked a
 *       modal dialog with xdg-dialog.
 *
 * Prints "configured" when shown; runs until stdin closes.
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
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"

static int W = 400, H = 300;
static uint32_t colour = 0xff0000ff;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_exporter_v2 *exporter;
static struct zxdg_importer_v2 *importer;
static struct xdg_wm_dialog_v1 *wm_dialog;
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
	size_t stride = (size_t) W * 4, size = stride * (size_t) H;
	int fd = memfd_create("buf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = colour;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void exported_handle(void *d, struct zxdg_exported_v2 *e, const char *handle) {
	say("handle %s", handle);
}
static const struct zxdg_exported_v2_listener exported_listener = {
	.handle = exported_handle,
};
static void imported_destroyed(void *d, struct zxdg_imported_v2 *i) { say("import-destroyed"); }
static const struct zxdg_imported_v2_listener imported_listener = {
	.destroyed = imported_destroyed,
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
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, zxdg_exporter_v2_interface.name) == 0)
		exporter = wl_registry_bind(r, name, &zxdg_exporter_v2_interface, 1);
	else if (strcmp(iface, zxdg_importer_v2_interface.name) == 0)
		importer = wl_registry_bind(r, name, &zxdg_importer_v2_interface, 1);
	else if (strcmp(iface, xdg_wm_dialog_v1_interface.name) == 0)
		wm_dialog = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static void command(const char *line) {}

int main(int argc, char **argv) {
	if (argc < 3 || (strcmp(argv[1], "import") == 0 && argc < 5)) {
		fprintf(stderr, "usage: dialog_client export APP_ID | import APP_ID HANDLE modal|plain\n");
		return 2;
	}
	int importing = strcmp(argv[1], "import") == 0;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !exporter || !importer || !wm_dialog) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	if (importing) {
		W = 200;
		H = 100;
		colour = 0xffff0000;
	}

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, argv[2]);
	xdg_toplevel_set_title(tl, argv[2]);
	if (importing) {
		struct zxdg_imported_v2 *imported =
			zxdg_importer_v2_import_toplevel(importer, argv[3]);
		zxdg_imported_v2_add_listener(imported, &imported_listener, NULL);
		zxdg_imported_v2_set_parent_of(imported, surface);
		if (strcmp(argv[4], "modal") == 0) {
			struct xdg_dialog_v1 *dialog = xdg_wm_dialog_v1_get_xdg_dialog(wm_dialog, tl);
			xdg_dialog_v1_set_modal(dialog);
		}
	} else {
		struct zxdg_exported_v2 *exported =
			zxdg_exporter_v2_export_toplevel(exporter, surface);
		zxdg_exported_v2_add_listener(exported, &exported_listener, NULL);
	}
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
