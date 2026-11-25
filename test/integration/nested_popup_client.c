/*
 * A panel (layer surface) opens a popup, and that popup a popup of its own,
 * as a tray menu with a submenu does. Prints "nested popup configured" and
 * exits 0 once the second one is shown (Waybox used to abort here).
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
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_wm_base *wm_base;
static struct wl_surface *bar, *p1s, *p2s;
static struct zwlr_layer_surface_v1 *ls;
static struct xdg_surface *x1, *x2;
static struct xdg_popup *pop1;
static int stage;

static struct wl_buffer *buf(int w, int h) {
	int fd = memfd_create("b", MFD_CLOEXEC);
	ftruncate(fd, w * h * 4);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, w * h * 4);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static struct xdg_positioner *pos(void) {
	struct xdg_positioner *p = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(p, 50, 50);
	xdg_positioner_set_anchor_rect(p, 0, 0, 10, 10);
	return p;
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	struct wl_surface *s = data;
	wl_surface_attach(s, buf(50, 50), 0, 0);
	wl_surface_commit(s);
	if (xs == x1 && stage == 1) {
		stage = 2;
		puts("popup 1 configured; creating nested popup");
		fflush(stdout);
		p2s = wl_compositor_create_surface(compositor);
		x2 = xdg_wm_base_get_xdg_surface(wm_base, p2s);
		static const struct xdg_surface_listener l = {.configure = xs_configure};
		xdg_surface_add_listener(x2, &l, p2s);
		(void) xdg_surface_get_popup(x2, x1, pos());
		wl_surface_commit(p2s);
	} else if (xs == x2) {
		puts("nested popup configured");
		fflush(stdout);
		exit(0);
	}
}
static const struct xdg_surface_listener xs_listener = {.configure = xs_configure};

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial,
		uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(l, serial);
	wl_surface_attach(bar, buf(w ? w : 100, h ? h : 30), 0, 0);
	wl_surface_commit(bar);
	if (stage == 0) {
		stage = 1;
		p1s = wl_compositor_create_surface(compositor);
		x1 = xdg_wm_base_get_xdg_surface(wm_base, p1s);
		xdg_surface_add_listener(x1, &xs_listener, p1s);
		pop1 = xdg_surface_get_popup(x1, NULL, pos());
		zwlr_layer_surface_v1_get_popup(ls, pop1);
		wl_surface_commit(p1s);
	}
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *l) {}
static const struct zwlr_layer_surface_v1_listener ls_listener = {ls_configure, ls_closed};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
	if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "zwlr_layer_shell_v1")) layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(iface, "xdg_wm_base")) wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg = {global, global_remove};

int main(void) {
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg, NULL);
	wl_display_roundtrip(dpy);
	bar = wl_compositor_create_surface(compositor);
	ls = zwlr_layer_shell_v1_get_layer_surface(layer_shell, bar, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "t");
	zwlr_layer_surface_v1_add_listener(ls, &ls_listener, NULL);
	zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ls, 0, 30);
	wl_surface_commit(bar);
	while (wl_display_dispatch(dpy) != -1) {}
	puts("connection lost");
	return 2;
}
