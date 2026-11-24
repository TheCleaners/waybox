/*
 * wlopm-like test client: "power_client off" or "power_client on" sets every
 * output's power mode and prints "mode on"/"mode off" for each output as
 * the compositor confirms, or "failed". Exits 0 when all confirmed.
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

static struct zwlr_output_power_manager_v1 *manager;
static struct wl_output *outputs[8];
static int n_outputs, confirmed, failed;
static uint32_t want;

static void power_mode(void *d, struct zwlr_output_power_v1 *p, uint32_t mode) {
	printf("mode %s\n", mode == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off");
	if (mode == want)
		confirmed++;
}
static void power_failed(void *d, struct zwlr_output_power_v1 *p) {
	printf("failed\n");
	failed = 1;
}
static const struct zwlr_output_power_v1_listener power_listener = {
	.mode = power_mode, .failed = power_failed,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_output_interface.name) == 0 && n_outputs < 8)
		outputs[n_outputs++] = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &zwlr_output_power_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
		fprintf(stderr, "usage: power_client on|off\n");
		return 2;
	}
	want = strcmp(argv[1], "on") == 0 ? ZWLR_OUTPUT_POWER_V1_MODE_ON
		: ZWLR_OUTPUT_POWER_V1_MODE_OFF;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager || n_outputs == 0) {
		fprintf(stderr, "no output power manager or outputs\n");
		return 1;
	}
	for (int i = 0; i < n_outputs; i++) {
		struct zwlr_output_power_v1 *p =
			zwlr_output_power_manager_v1_get_output_power(manager, outputs[i]);
		zwlr_output_power_v1_add_listener(p, &power_listener, NULL);
		zwlr_output_power_v1_set_mode(p, want);
	}
	/* The initial mode event, then the one after our request. */
	for (int i = 0; i < 4 && confirmed < n_outputs && !failed; i++)
		wl_display_roundtrip(display);
	fflush(stdout);
	return confirmed >= n_outputs && !failed ? 0 : 1;
}
