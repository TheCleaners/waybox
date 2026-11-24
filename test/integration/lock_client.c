/*
 * Minimal ext-session-lock-v1 client for the session-lock integration test.
 *
 * Locks the session and paints every output solid green (0xff00ff00).
 * Prints "locked" or "finished" on stdout as the compositor answers. Reading
 * "unlock" on stdin unlocks and exits 0; being killed simulates a crashed
 * locker. Exits 2 if the compositor refused the lock ("finished").
 * Prints "key" / "button" for keyboard keys and pointer buttons it receives,
 * so the test can check which input reaches the lock screen.
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

#include "ext-session-lock-v1-client-protocol.h"

#define MAX_OUTPUTS 8

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct ext_session_lock_manager_v1 *lock_manager;
static struct wl_output *outputs[MAX_OUTPUTS];
static int num_outputs;
static int finished;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd,
		uint32_t size) {
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *s) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		puts("key");
		fflush(stdout);
	}
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
		uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener keyboard_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat,
};

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		puts("button");
		fflush(stdout);
	}
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion,
	.button = p_button, .axis = p_axis,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities, .name = seat_name,
};

struct lock_surface {
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
};

static struct wl_buffer *solid_buffer(uint32_t width, uint32_t height) {
	size_t stride = width * 4, size = stride * height;
	int fd = memfd_create("lock-buffer", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0) {
		perror("memfd");
		exit(1);
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	for (size_t i = 0; i < size / 4; i++)
		pixels[i] = 0xff00ff00;
	munmap(pixels, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t) width,
			(int32_t) height, (int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void surface_configure(void *data,
		struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct lock_surface *ls = data;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	wl_surface_attach(ls->surface, solid_buffer(width, height), 0, 0);
	wl_surface_damage_buffer(ls->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(ls->surface);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
	.configure = surface_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	puts("locked");
	fflush(stdout);
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	puts("finished");
	fflush(stdout);
	finished = 1;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		lock_manager = wl_registry_bind(registry, name,
				&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 2);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0 &&
			num_outputs < MAX_OUTPUTS) {
		outputs[num_outputs++] = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
	}
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
	if (compositor == NULL || shm == NULL || lock_manager == NULL) {
		fprintf(stderr, "missing wl_compositor, wl_shm or ext_session_lock_manager_v1\n");
		return 1;
	}

	struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(lock_manager);
	ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
	static struct lock_surface surfaces[MAX_OUTPUTS];
	for (int i = 0; i < num_outputs; i++) {
		surfaces[i].surface = wl_compositor_create_surface(compositor);
		surfaces[i].lock_surface = ext_session_lock_v1_get_lock_surface(lock,
				surfaces[i].surface, outputs[i]);
		ext_session_lock_surface_v1_add_listener(surfaces[i].lock_surface,
				&surface_listener, &surfaces[i]);
	}

	struct pollfd fds[2] = {
		{.fd = wl_display_get_fd(display), .events = POLLIN},
		{.fd = STDIN_FILENO, .events = POLLIN},
	};
	while (!finished) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0) {
			wl_display_cancel_read(display);
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0)
				return 1;
		} else {
			wl_display_cancel_read(display);
		}
		if (wl_display_dispatch_pending(display) < 0)
			return 1;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			char line[64];
			if (fgets(line, sizeof(line), stdin) == NULL)
				fds[1].fd = -1;  /* stdin closed: keep the lock held */
			else if (strncmp(line, "unlock", 6) == 0) {
				ext_session_lock_v1_unlock_and_destroy(lock);
				wl_display_roundtrip(display);
				return 0;
			}
		}
	}
	return finished ? 2 : 0;
}
