/*
 * xdg-toplevel-icon test client: a window (the app id given on the command
 * line), framed by the compositor, whose icon is pixels, not a name: a 32 px buffer of one colour
 * (the second argument, 0xAARRGGBB; magenta if none), given with
 * add_buffer. Prints "configured" once shown, "no-icon-protocol" (exit 3)
 * if the compositor doesn't offer it. Runs until killed.
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
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct xdg_toplevel_icon_manager_v1 *icons;
static struct zxdg_decoration_manager_v1 *decorations;
static struct wl_surface *surface;
static int configured;

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void icon_size(void *data, struct xdg_toplevel_icon_manager_v1 *m, int32_t size) {}
static void icon_done(void *data, struct xdg_toplevel_icon_manager_v1 *m) {}
static const struct xdg_toplevel_icon_manager_v1_listener icons_listener = {
	.icon_size = icon_size, .done = icon_done};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
		decorations = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name) == 0) {
		icons = wl_registry_bind(r, name, &xdg_toplevel_icon_manager_v1_interface, 1);
		xdg_toplevel_icon_manager_v1_add_listener(icons, &icons_listener, NULL);
	}
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {.global = global,
	.global_remove = global_remove};

static struct wl_buffer *buffer(int w, int h, uint32_t colour) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("icon-buffer", 0);
	if (fd < 0 || ftruncate(fd, size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++)
		px[i] = colour;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		wl_surface_attach(surface, buffer(300, 200, 0xff404040), 0, 0);
		wl_surface_commit(surface);
		configured = 1;
		printf("configured\n");
		fflush(stdout);
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure};

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {}
static void tl_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener tl_listener = {.configure = tl_configure,
	.close = tl_close};

int main(int argc, char **argv) {
	const char *app_id = argc > 1 ? argv[1] : "pixicon";
	uint32_t colour = argc > 2 ? (uint32_t) strtoul(argv[2], NULL, 16) : 0xffff00ff;
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL)
		return 1;
	if (icons == NULL) {
		printf("no-icon-protocol\n");
		return 3;
	}
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, app_id);
	/* A frame from the compositor (its titlebar shows the icon). */
	if (decorations != NULL)
		zxdg_toplevel_decoration_v1_set_mode(
				zxdg_decoration_manager_v1_get_toplevel_decoration(decorations, tl),
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	/* Its icon: pixels only, no name (nothing to look up instead). */
	struct xdg_toplevel_icon_v1 *icon = xdg_toplevel_icon_manager_v1_create_icon(icons);
	xdg_toplevel_icon_v1_add_buffer(icon, buffer(32, 32, colour), 1);
	xdg_toplevel_icon_manager_v1_set_icon(icons, tl, icon);
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0)
		;
	return 0;
}
