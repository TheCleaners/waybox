/*
 * wlr-screencopy test client: captures the first output once into shared
 * memory and prints "size W H" and "pixel X Y R G B" for the pixel asked
 * for, or "failed" / "no-screencopy".
 *
 *   screencopy_client X Y
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *manager;
static uint32_t fmt, width, height, stride;
static int have_shm_format, buffer_done, done;
static void *pixels;
static struct wl_buffer *buffer;
static struct zwlr_screencopy_frame_v1 *frame;

static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
		uint32_t w, uint32_t h, uint32_t s) {
	fmt = format;
	width = w;
	height = h;
	stride = s;
	have_shm_format = 1;
}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo,
		uint32_t ns) {
	done = 1;
}
static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
	printf("failed\n");
	exit(1);
}
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h) {}
static void f_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
		uint32_t w, uint32_t h) {}
static void f_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
	buffer_done = 1;
}
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = f_buffer, .flags = f_flags, .ready = f_ready, .failed = f_failed,
	.damage = f_damage, .linux_dmabuf = f_dmabuf, .buffer_done = f_buffer_done,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_output_interface.name) == 0 && output == NULL)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface,
				version < 3 ? version : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	int px = argc > 2 ? atoi(argv[1]) : 0, py = argc > 2 ? atoi(argv[2]) : 0;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (manager == NULL) {
		printf("no-screencopy\n");
		return 1;
	}
	if (!shm || !output)
		return 1;
	frame = zwlr_screencopy_manager_v1_capture_output(manager, 0, output);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
	while (!buffer_done && wl_display_dispatch(display) >= 0) {
	}
	if (!have_shm_format)
		return 1;
	size_t size = (size_t) stride * height;
	char name[] = "/tmp/wb-screencopy-XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0)
		return 1;
	unlink(name);
	if (ftruncate(fd, (off_t) size) < 0)
		return 1;
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		return 1;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t) width, (int32_t) height,
			(int32_t) stride, fmt);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(frame, buffer);
	while (!done && wl_display_dispatch(display) >= 0) {
	}
	printf("size %u %u\n", width, height);
	if (px >= 0 && py >= 0 && (uint32_t) px < width && (uint32_t) py < height) {
		/* XRGB/ARGB8888 (little-endian: B G R A in memory). */
		uint8_t *p = (uint8_t *) pixels + (size_t) py * stride + (size_t) px * 4;
		if (fmt == WL_SHM_FORMAT_XBGR8888 || fmt == WL_SHM_FORMAT_ABGR8888)
			printf("pixel %d %d %u %u %u\n", px, py, p[0], p[1], p[2]);
		else
			printf("pixel %d %d %u %u %u\n", px, py, p[2], p[1], p[0]);
	}
	fflush(stdout);
	return 0;
}
