/*
 * xdg-activation test client: a window with the app_id given on the command
 * line. Prints "configured" once shown. On stdin, "activate" gets a token
 * with no input behind it (no serial: as a program on its own, a link handed
 * to an open browser), asks to be activated with it, then prints "asked".
 * Runs until killed.
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

#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;
static struct wl_surface *surface;
static int configured;

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (strcmp(iface, xdg_activation_v1_interface.name) == 0)
		activation = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {.global = global,
	.global_remove = global_remove};

static struct wl_buffer *buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("activate", 0);
	if (fd < 0 || ftruncate(fd, size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++)
		px[i] = 0xff40a040;
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
		wl_surface_attach(surface, buffer(300, 200), 0, 0);
		wl_surface_commit(surface);
		configured = 1;
		printf("configured\n");
		fflush(stdout);
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure};

static char token[256];

static void token_done(void *data, struct xdg_activation_token_v1 *t, const char *value) {
	snprintf(token, sizeof(token), "%s", value);
}
static const struct xdg_activation_token_v1_listener token_listener = {.done = token_done};

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {}
static void tl_close(void *data, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener tl_listener = {.configure = tl_configure,
	.close = tl_close};

int main(int argc, char **argv) {
	const char *app_id = argc > 1 ? argv[1] : "activate";
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || wm_base == NULL)
		return 1;
	if (activation == NULL) {
		printf("no-activation\n");
		return 3;
	}
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, app_id);
	wl_surface_commit(surface);

	struct pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {0, POLLIN, 0}};
	char line[64];
	for (;;) {
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0)
			break;
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(display) < 0)
				break;
		}
		if (fds[1].revents & POLLIN) {
			if (fgets(line, sizeof(line), stdin) == NULL)
				break;
			if (strncmp(line, "activate", 8) == 0) {
				/* A token no input made: no serial set. */
				struct xdg_activation_token_v1 *t =
					xdg_activation_v1_get_activation_token(activation);
				token[0] = 0;
				xdg_activation_token_v1_add_listener(t, &token_listener, NULL);
				xdg_activation_token_v1_set_surface(t, surface);
				xdg_activation_token_v1_commit(t);
				while (token[0] == 0 && wl_display_dispatch(display) >= 0)
					;
				xdg_activation_token_v1_destroy(t);
				xdg_activation_v1_activate(activation, token, surface);
				wl_display_roundtrip(display);
				printf("asked\n");
				fflush(stdout);
			}
		}
	}
	return 0;
}
