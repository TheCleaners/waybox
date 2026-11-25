/*
 * pointer-gestures test client: binds zwp_pointer_gestures_v1, subscribes
 * to swipe, pinch and hold on its pointer, and prints "gestures N" (the
 * version) once that went through without an error, or "no gestures".
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "pointer-gestures-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_pointer_gestures_v1 *gestures;
static uint32_t version;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t v) {
	if (strcmp(iface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	} else if (strcmp(iface, zwp_pointer_gestures_v1_interface.name) == 0) {
		version = v < 3 ? v : 3;
		gestures = wl_registry_bind(r, name, &zwp_pointer_gestures_v1_interface, version);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !gestures) {
		printf("no gestures\n");
		return 1;
	}
	struct wl_pointer *pointer = wl_seat_get_pointer(seat);
	zwp_pointer_gestures_v1_get_swipe_gesture(gestures, pointer);
	zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer);
	if (version >= 3)
		zwp_pointer_gestures_v1_get_hold_gesture(gestures, pointer);
	if (wl_display_roundtrip(display) < 0) {
		printf("error\n");
		return 1;
	}
	printf("gestures %u\n", version);
	return 0;
}
