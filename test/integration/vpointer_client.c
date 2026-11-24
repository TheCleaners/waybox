/*
 * Virtual pointer for tests that need a held button (wlrctl only clicks).
 * Commands on stdin, one per line: "move DX DY", "press", "release".
 * Prints "ok" after each, once the compositor has processed it.
 */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
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
	if (!seat || !manager) {
		fprintf(stderr, "no virtual pointer manager\n");
		return 1;
	}
	struct zwlr_virtual_pointer_v1 *ptr =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
	uint32_t t = 0;
	char line[64];
	while (fgets(line, sizeof(line), stdin)) {
		double dx, dy;
		t += 10;
		if (sscanf(line, "move %lf %lf", &dx, &dy) == 2) {
			zwlr_virtual_pointer_v1_motion(ptr, t, wl_fixed_from_double(dx),
					wl_fixed_from_double(dy));
		} else if (strncmp(line, "press", 5) == 0) {
			zwlr_virtual_pointer_v1_button(ptr, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
		} else if (strncmp(line, "release", 7) == 0) {
			zwlr_virtual_pointer_v1_button(ptr, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		} else {
			continue;
		}
		zwlr_virtual_pointer_v1_frame(ptr);
		wl_display_roundtrip(display);
		printf("ok\n");
		fflush(stdout);
	}
	return 0;
}
