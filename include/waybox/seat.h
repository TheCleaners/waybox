#ifndef _WB_SEAT_H
#define _WB_SEAT_H

#include <vector>
#include "waybox/wlroots.hpp"

#include "waybox/action.hpp"
#include "waybox/listener.hpp"

struct wb_server;

struct wb_seat {
	struct wlr_seat *seat = nullptr;
	struct wb_server *server = nullptr;

	struct wlr_layer_surface_v1 *focused_layer = nullptr;

	struct wl_list keyboards;
	/* Pointer devices, so Reconfigure can apply changed <libinput>
	 * settings to them. */
	std::vector<struct wb_pointer *> pointers;
	/* Touchscreens attached: the seat offers touch while there are any. */
	int touch_devices = 0;
	/* WAYBOX_TEST_TOUCH=1 at start: a fake touchscreen for the tests,
	 * driven over IPC (test_touch). Never there otherwise. */
	struct wlr_touch *test_touch = nullptr;

	wb::Listener request_set_primary_selection;
	wb::Listener request_set_selection;
	/* Drag-and-drop between clients: starting one, and its icon. */
	wb::Listener request_start_drag;
	wb::Listener start_drag;
	wb::Listener drag_destroy;

	~wb_seat();
};

struct wb_pointer {
	struct wlr_input_device *device = nullptr;
	wb::Listener destroy;
};

struct wb_keyboard {
	struct wl_list link;
	struct wb_server *server = nullptr;
	struct wlr_keyboard *keyboard = nullptr;

	wb::Listener destroy;
	wb::Listener modifiers;
	wb::Listener key;

	/* A held key whose binding says repeat="yes": its actions run again
	 * after the keyboard's repeat delay, then at its rate, until any key
	 * event from this keyboard (its release, another key, a modifier). */
	struct wl_event_source *repeat_timer = nullptr;
	std::vector<wb::Action> repeat_actions;
	uint32_t repeat_modifiers = 0;

	void on_modifiers(void *data);
	void on_key(void *data);
	void start_repeat(std::vector<wb::Action> actions, uint32_t modifiers);
	void stop_repeat();
	~wb_keyboard() { stop_repeat(); }
};

struct wb_seat *wb_seat_create(struct wb_server *server);

/* True for devices created through virtual-keyboard/-pointer (wtype, wlrctl,
 * on-screen keyboards, remote-desktop tools). */
bool wb_input_is_virtual(struct wlr_input_device *device);
/* True if input from `device` must be dropped under the <virtualInput>
 * policy (see config.h). */
bool wb_input_blocked(struct wb_server *server, struct wlr_input_device *device);
/* The seat's keyboard, if <virtualInput> blocks it now (locked, or the
 * policy changed): another allowed one, or none. */
void seat_recheck_keyboard(struct wb_server *server);
/* The test touchscreen (only with WAYBOX_TEST_TOUCH=1): "down", "motion",
 * "up" or "frame" for a touch point at (x, y) in 0..1 of the layout.
 * False without it. */
void seat_init_test_touch(struct wb_server *server);   /* after the cursor */
void seat_finish_test_touch(struct wb_server *server); /* before the cursor goes */
bool seat_test_touch(struct wb_server *server, const std::string &what, int32_t id,
		double x, double y);
void seat_focus_surface(struct wb_seat *seat, struct wlr_surface *surface);
void seat_set_focus_layer(struct wb_seat *seat, struct wlr_layer_surface_v1 *layer);

/* Alt+Tab task-switcher lifecycle (defined in seat.cpp). commit focuses the
 * selected window and drops the OSD; cancel drops it without changing focus.
 * Both are no-ops when no switcher is active. */
void wb_switcher_commit(struct wb_server *server);
void wb_switcher_cancel(struct wb_server *server);
/* A window the switcher lists is going: its row goes, the switcher stays
 * (closed from it, with Delete); with none left, it's cancelled. */
void wb_switcher_forget(struct wb_server *server, struct wb_toplevel *toplevel);
/* A pointer button at layout (x, y) while the switcher is up: true if it
 * was on its list (and so taken). */
bool wb_switcher_click(struct wb_server *server, double x, double y, uint32_t button,
		bool pressed);
/* Reconfigure: apply the keymap and pointer settings from the new rc.xml to
 * the devices already connected. */
void seat_reconfigure_input(struct wb_server *server);

#endif
