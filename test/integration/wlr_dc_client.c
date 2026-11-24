/*
 * A clipboard manager speaking only zwlr_data_control (as cliphist,
 * clipman and older wl-clipboard do): prints "ready", then "selection
 * TEXT" each time the clipboard changes to text.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *manager;
static int has_text;

static void offer_mime(void *d, struct zwlr_data_control_offer_v1 *o, const char *mime) {
	if (strcmp(mime, "text/plain;charset=utf-8") == 0 || strcmp(mime, "text/plain") == 0)
		has_text = 1;
}
static const struct zwlr_data_control_offer_v1_listener offer_listener = {
	.offer = offer_mime,
};

static void dev_data_offer(void *d, struct zwlr_data_control_device_v1 *dev,
		struct zwlr_data_control_offer_v1 *offer) {
	has_text = 0;
	zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, NULL);
}
static void dev_selection(void *d, struct zwlr_data_control_device_v1 *dev,
		struct zwlr_data_control_offer_v1 *offer) {
	if (offer == NULL || !has_text)
		return;
	int fds[2];
	if (pipe(fds) != 0)
		return;
	zwlr_data_control_offer_v1_receive(offer, "text/plain;charset=utf-8", fds[1]);
	close(fds[1]);
	wl_display_flush(display);
	char buf[256] = {0};
	ssize_t n, total = 0;
	while ((n = read(fds[0], buf + total, sizeof(buf) - 1 - total)) > 0)
		total += n;
	close(fds[0]);
	printf("selection %s\n", buf);
	fflush(stdout);
	zwlr_data_control_offer_v1_destroy(offer);
}
static void dev_finished(void *d, struct zwlr_data_control_device_v1 *dev) {}
static void dev_primary(void *d, struct zwlr_data_control_device_v1 *dev,
		struct zwlr_data_control_offer_v1 *offer) {}
static const struct zwlr_data_control_device_v1_listener dev_listener = {
	.data_offer = dev_data_offer, .selection = dev_selection,
	.finished = dev_finished, .primary_selection = dev_primary,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, 2);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(void) {
	display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !manager) {
		printf("no zwlr_data_control_manager_v1\n");
		fflush(stdout);
		return 1;
	}
	struct zwlr_data_control_device_v1 *dev =
		zwlr_data_control_manager_v1_get_data_device(manager, seat);
	zwlr_data_control_device_v1_add_listener(dev, &dev_listener, NULL);
	wl_display_roundtrip(display);
	printf("ready\n");
	fflush(stdout);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
