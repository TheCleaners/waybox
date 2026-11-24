/*
 * Minimal wlr-layer-shell client for the output-hotplug integration test.
 *
 * Puts a 30px top bar (like a panel) on every output and keeps redrawing it
 * every frame, so it keeps committing after an output disappears, which is
 * when a compositor holding a stale output pointer crashes. Prints "configured"
 * for each bar the compositor configures and "closed" for each bar it closes.
 * Runs until killed.
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

#define MAX_BARS 8
#define BAR_HEIGHT 30

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_output *outputs[MAX_BARS];
static int num_outputs;

struct bar {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	uint32_t tick;
	int closed;
};
static struct bar bars[MAX_BARS];

static struct wl_buffer *bar_buffer(uint32_t width, uint32_t height, uint32_t color) {
	size_t stride = width * 4, size = stride * height;
	int fd = memfd_create("bar", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0) {
		perror("memfd");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	for (size_t i = 0; i < size / 4; i++)
		px[i] = color;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t) width,
			(int32_t) height, (int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void draw(struct bar *bar);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
	struct bar *bar = data;
	if (!bar->closed)
		draw(bar);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw(struct bar *bar) {
	/* Alternate the colour so every frame is a real content change. */
	uint32_t color = (bar->tick++ & 1) ? 0xff202020 : 0xff303030;
	struct wl_buffer *buffer = bar_buffer(bar->width, bar->height, color);
	wl_surface_attach(bar->surface, buffer, 0, 0);
	wl_surface_damage_buffer(bar->surface, 0, 0, INT32_MAX, INT32_MAX);
	struct wl_callback *cb = wl_surface_frame(bar->surface);
	wl_callback_add_listener(cb, &frame_listener, bar);
	wl_surface_commit(bar->surface);
	wl_buffer_destroy(buffer);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct bar *bar = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	bar->width = width ? width : 100;
	bar->height = height ? height : BAR_HEIGHT;
	puts("configured");
	fflush(stdout);
	draw(bar);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
	struct bar *bar = data;
	bar->closed = 1;
	zwlr_layer_surface_v1_destroy(ls);
	wl_surface_destroy(bar->surface);
	puts("closed");
	fflush(stdout);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	else if (strcmp(interface, wl_output_interface.name) == 0 && num_outputs < MAX_BARS)
		outputs[num_outputs++] = wl_registry_bind(registry, name, &wl_output_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (compositor == NULL || shm == NULL || layer_shell == NULL) {
		fprintf(stderr, "missing wl_compositor, wl_shm or zwlr_layer_shell_v1\n");
		return 1;
	}
	for (int i = 0; i < num_outputs; i++) {
		struct bar *bar = &bars[i];
		bar->surface = wl_compositor_create_surface(compositor);
		bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
				bar->surface, outputs[i], ZWLR_LAYER_SHELL_V1_LAYER_TOP, "test-bar");
		zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_listener, bar);
		zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, BAR_HEIGHT);
		zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, BAR_HEIGHT);
		wl_surface_commit(bar->surface);
	}
	while (wl_display_dispatch(display) != -1) {
	}
	return 0;
}
