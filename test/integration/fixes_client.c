/*
 * wl_fixes test client. Prints:
 *   "version N"            the wl_fixes version offered (bound at it);
 *   "registry-destroyed"   a second registry destroyed with destroy_registry
 *                          and the connection still fine after a roundtrip;
 *   "ack-error IFACE CODE" acknowledging the removal of a global that wasn't
 *                          removed (wl_compositor's): the protocol error.
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static struct wl_fixes *fixes;
static uint32_t fixes_version, compositor_name;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_fixes_interface.name) == 0) {
		fixes_version = version;
		fixes = wl_registry_bind(r, name, &wl_fixes_interface, version);
	} else if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor_name = name;
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
	if (fixes == NULL) {
		printf("no wl_fixes\n");
		return 1;
	}
	printf("version %u\n", fixes_version);
	fflush(stdout);

	struct wl_registry *extra = wl_display_get_registry(display);
	wl_fixes_destroy_registry(fixes, extra);
	wl_registry_destroy(extra);  /* the proxy */
	if (wl_display_roundtrip(display) >= 0) {
		printf("registry-destroyed\n");
		fflush(stdout);
	}

	if (fixes_version >= 2) {
		wl_fixes_ack_global_remove(fixes, reg, compositor_name);
		wl_display_roundtrip(display);
		const struct wl_interface *iface = NULL;
		uint32_t code = wl_display_get_protocol_error(display, &iface, NULL);
		printf("ack-error %s %u\n", iface ? iface->name : "none", code);
		fflush(stdout);
	}
	return 0;
}
