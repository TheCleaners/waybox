/*
 * Drag-and-drop test client: a 300x200 window, as "source" or "target"
 * (its app_id is dnd-source / dnd-target).
 *
 * source: a button press starts a drag offering text/plain with a 24x24
 *   icon. Prints "drag-started", "icon-on-output" (the icon surface entered
 *   an output: it's shown), "sent" (data written to the target),
 *   "drop-performed", "finished" or "cancelled".
 * target: prints "enter" when a drag comes over it (accepting text/plain),
 *   and "dropped TEXT" with what it received.
 * tearoff: as source, but like a browser tab torn off: the drag also gets
 *   an xdg_toplevel_drag, and a new 200x100 window (app_id dnd-torn) is
 *   attached at offset 20,10 and mapped during the drag. Prints
 *   "torn-mapped" once it's shown.
 * Both print "configured" once shown. Runs until killed.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"

#define MIME "text/plain;charset=utf-8"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *device;
static struct wl_surface *surface;
static int is_source, is_tearoff, configured;
static struct xdg_toplevel_drag_manager_v1 *tdm;

static void say(const char *s) {
	printf("%s\n", s);
	fflush(stdout);
}

static struct wl_buffer *make_buffer(int W, int H, uint32_t color) {
	size_t stride = W * 4, size = stride * H;
	int fd = memfd_create("buf", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t) size) < 0)
		exit(1);
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = color;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H,
			(int32_t) stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

/* ---- source side ---- */
static void src_target(void *d, struct wl_data_source *s, const char *mime) {}
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
	const char *text = "hello-dnd";
	if (write(fd, text, strlen(text)) < 0)
		perror("write");
	close(fd);
	say("sent");
}
static void src_cancelled(void *d, struct wl_data_source *s) {
	say("cancelled");
	wl_data_source_destroy(s);
}
static void src_drop_performed(void *d, struct wl_data_source *s) { say("drop-performed"); }
static void src_finished(void *d, struct wl_data_source *s) {
	say("finished");
	wl_data_source_destroy(s);
}
static void src_action(void *d, struct wl_data_source *s, uint32_t a) {}
static const struct wl_data_source_listener src_listener = {
	.target = src_target, .send = src_send, .cancelled = src_cancelled,
	.dnd_drop_performed = src_drop_performed, .dnd_finished = src_finished,
	.action = src_action,
};

static void icon_enter(void *d, struct wl_surface *s, struct wl_output *o) { say("icon-on-output"); }
static void icon_leave(void *d, struct wl_surface *s, struct wl_output *o) {}
static const struct wl_surface_listener icon_listener = {
	.enter = icon_enter, .leave = icon_leave,
};

/* ---- the torn-off window ---- */
static struct wl_surface *torn_surface;
static int torn_shown;
static void torn_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!torn_shown) {
		wl_surface_attach(torn_surface, make_buffer(200, 100, 0xffa03060), 0, 0);
		wl_surface_commit(torn_surface);
		torn_shown = 1;
		say("torn-mapped");
	}
}
static const struct xdg_surface_listener torn_xs_listener = { .configure = torn_configure };
static void torn_tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *s) {}
static void torn_tl_close(void *d, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener torn_tl_listener = {
	.configure = torn_tl_configure, .close = torn_tl_close,
};

static void tear_off(struct wl_data_source *src) {
	struct xdg_toplevel_drag_v1 *drag =
		xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(tdm, src);
	torn_surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, torn_surface);
	xdg_surface_add_listener(xs, &torn_xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &torn_tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, "dnd-torn");
	xdg_toplevel_drag_v1_attach(drag, tl, 20, 10);
	wl_surface_commit(torn_surface);
}

static void start_drag(uint32_t serial) {
	struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(src, &src_listener, NULL);
	wl_data_source_offer(src, MIME);
	wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	struct wl_surface *icon = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(icon, &icon_listener, NULL);
	struct wl_data_source *keep = src;
	wl_data_device_start_drag(device, src, surface, icon, serial);
	if (is_tearoff)
		tear_off(keep);
	wl_surface_attach(icon, make_buffer(24, 24, 0xffe0a020), 0, 0);
	wl_surface_commit(icon);
	say("drag-started");
}

/* ---- target side ---- */
static struct wl_data_offer *current;
static void offer_offer(void *d, struct wl_data_offer *o, const char *mime) {}
static void offer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) {}
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) {}
static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_offer, .source_actions = offer_source_actions, .action = offer_action,
};
static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
	if (is_source || o == NULL)
		return;
	current = o;
	wl_data_offer_accept(o, serial, MIME);
	wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	say("enter");
}
static void dd_leave(void *d, struct wl_data_device *dd) {
	if (current != NULL && !is_source) {
		/* Not dropped here. */
	}
}
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void dd_drop(void *d, struct wl_data_device *dd) {
	if (is_source || current == NULL)
		return;
	int fds[2];
	if (pipe(fds) < 0)
		return;
	wl_data_offer_receive(current, MIME, fds[1]);
	close(fds[1]);
	wl_display_flush(wl_proxy_get_display((struct wl_proxy *) dd));
	char buf[64] = {0};
	size_t n = 0;
	ssize_t r;
	while (n < sizeof buf - 1 && (r = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0)
		n += (size_t) r;
	close(fds[0]);
	printf("dropped %s\n", buf);
	fflush(stdout);
	wl_data_offer_finish(current);
	wl_data_offer_destroy(current);
	current = NULL;
}
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {}
static const struct wl_data_device_listener dd_listener = {
	.data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave,
	.motion = dd_motion, .drop = dd_drop, .selection = dd_selection,
};

/* ---- pointer ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) { say("pointer-enter"); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t button, uint32_t state) {
	say(state == WL_POINTER_BUTTON_STATE_PRESSED ? "press" : "release");
	if (is_source && state == WL_POINTER_BUTTON_STATE_PRESSED)
		start_drag(serial);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion,
	.button = p_button, .axis = p_axis,
};

/* ---- window ---- */
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		wl_surface_attach(surface, make_buffer(300, 200, is_source ? 0xff3060a0 : 0xff30a060), 0, 0);
		wl_surface_commit(surface);
		configured = 1;
		say("configured");
	}
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tl_listener = {
	.configure = tl_configure, .close = tl_close,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 4);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (strcmp(iface, xdg_toplevel_drag_manager_v1_interface.name) == 0)
		tdm = wl_registry_bind(r, name, &xdg_toplevel_drag_manager_v1_interface, 1);
	else if (strcmp(iface, wl_output_interface.name) == 0)
		wl_registry_bind(r, name, &wl_output_interface, 1);  /* for surface enter */
	else if (strcmp(iface, wl_data_device_manager_interface.name) == 0)
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	is_tearoff = argc > 1 && strcmp(argv[1], "tearoff") == 0;
	is_source = is_tearoff || (argc > 1 && strcmp(argv[1], "source") == 0);
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !ddm)
		return 1;
	if (is_tearoff && !tdm) {
		say("no-toplevel-drag");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	device = wl_data_device_manager_get_data_device(ddm, seat);
	wl_data_device_add_listener(device, &dd_listener, NULL);
	wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_app_id(tl, is_source ? "dnd-source" : "dnd-target");
	wl_surface_commit(surface);
	while (wl_display_dispatch(display) >= 0) {
	}
	return 0;
}
