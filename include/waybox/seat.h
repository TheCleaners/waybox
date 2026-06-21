#ifndef _WB_SEAT_H
#define _WB_SEAT_H

#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"

struct wb_server;

struct wb_seat {
	struct wlr_seat *seat = nullptr;

	struct wlr_layer_surface_v1 *focused_layer = nullptr;

	struct wl_list keyboards;

	wb::Listener request_set_primary_selection;
	wb::Listener request_set_selection;

	~wb_seat();
};

struct wb_keyboard {
	struct wl_list link;
	struct wb_server *server = nullptr;
	struct wlr_keyboard *keyboard = nullptr;

	wb::Listener destroy;
	wb::Listener modifiers;
	wb::Listener key;

	void on_modifiers(void *data);
	void on_key(void *data);
};

struct wb_seat *wb_seat_create(struct wb_server *server);
void seat_focus_surface(struct wb_seat *seat, struct wlr_surface *surface);
void seat_set_focus_layer(struct wb_seat *seat, struct wlr_layer_surface_v1 *layer);

/* Alt+Tab task-switcher lifecycle (defined in seat.cpp). commit focuses the
 * selected window and drops the OSD; cancel drops it without changing focus.
 * Both are no-ops when no switcher is active. */
void wb_switcher_commit(struct wb_server *server);
void wb_switcher_cancel(struct wb_server *server);
#endif
