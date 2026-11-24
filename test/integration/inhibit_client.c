/*
 * Idle-inhibit test client: creates a surface, then reads commands on
 * stdin: "new" makes an idle inhibitor (printed as "made N"), "del N"
 * destroys inhibitor N. Prints "ready" once connected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct zwp_idle_inhibit_manager_v1 *manager;
static struct zwp_idle_inhibitor_v1 *inhibitors[16];
static int count;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
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
	if (!compositor || !manager) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	wl_display_roundtrip(display);
	printf("ready\n");
	fflush(stdout);
	char line[64];
	while (fgets(line, sizeof(line), stdin)) {
		int n;
		if (strncmp(line, "new", 3) == 0 && count < 16) {
			inhibitors[count] = zwp_idle_inhibit_manager_v1_create_inhibitor(manager, surface);
			printf("made %d\n", count++);
		} else if (sscanf(line, "del %d", &n) == 1 && n >= 0 && n < count && inhibitors[n]) {
			zwp_idle_inhibitor_v1_destroy(inhibitors[n]);
			inhibitors[n] = NULL;
			printf("deleted %d\n", n);
		}
		wl_display_roundtrip(display);
		fflush(stdout);
	}
	return 0;
}
