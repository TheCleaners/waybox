#include <linux/input-event-codes.h>

#include "waybox/cursor.h"
#include "waybox/xdg_shell.h"

void reset_cursor_mode(struct wb_server *server) {
	/* Reset the cursor mode to passthrough */
	server->cursor->cursor_mode = WB_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct wb_server *server) {
	/* Move the grabbed toplevel to the new position. */
	struct wb_toplevel *toplevel = server->grabbed_toplevel;
	if (toplevel->scene_tree->node.type == WLR_SCENE_NODE_TREE) {
		toplevel->geometry.x = server->cursor->cursor->x - server->grab_x;
		toplevel->geometry.y = server->cursor->cursor->y - server->grab_y;
		/* Don't let the window be dragged over a panel's reserved area. */
		constrain_toplevel_to_usable_area(toplevel);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
				toplevel->geometry.x, toplevel->geometry.y);
	}
}

static void process_cursor_resize(struct wb_server *server) {
	struct wb_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->cursor->x - server->grab_x;
	double border_y = server->cursor->cursor->y - server->grab_y;
	int new_left = server->grab_geo_box.x;
	int new_right = server->grab_geo_box.x + server->grab_geo_box.width;
	int new_top = server->grab_geo_box.y;
	int new_bottom = server->grab_geo_box.y + server->grab_geo_box.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;
	toplevel->geometry.x = new_left - geo_box.x;
	toplevel->geometry.y = new_top - geo_box.y;
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
				toplevel->geometry.x, toplevel->geometry.y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(struct wb_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor->cursor_mode == WB_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor->cursor_mode == WB_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat->seat;
	struct wlr_surface *surface = NULL;
	struct wb_toplevel *toplevel = get_toplevel_at(server,
			server->cursor->cursor->x, server->cursor->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(
				server->cursor->cursor, server->cursor->xcursor_manager, "default");
	}
	if (surface) {
		/*
		 * "Enter" the surface if necessary. This lets the client know that the
		 * cursor has entered one of its surfaces.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
}


void wb_cursor::on_motion(void *data) {
	auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
	wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void wb_cursor::on_motion_absolute(void *data) {
	auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

void wb_cursor::on_pointer_focus_change(void *data) {
	/* Raised when pointer focus changes, including when the client is closed;
	 * fall back to the default cursor image if there is no target surface. */
	auto *event = static_cast<struct wlr_seat_pointer_focus_change_event *>(data);
	if (event->new_surface == nullptr) {
		wlr_cursor_set_xcursor(cursor, xcursor_manager, "default");
	}
}

void wb_cursor::on_button(void *data) {
	auto *event = static_cast<struct wlr_pointer_button_event *>(data);
	struct wlr_seat *seat = server->seat->seat;
	double sx, sy;
	struct wlr_surface *surface = nullptr;
	struct wb_toplevel *toplevel = get_toplevel_at(server,
			cursor->x, cursor->y, &surface, &sx, &sy);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* Alt + drag moves (left) or resizes (right) the window under the
		 * pointer regardless of its decorations, matching Openbox's default
		 * frame mouse bindings. The button is consumed, not forwarded. */
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
		uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		if (toplevel != nullptr && (modifiers & WLR_MODIFIER_ALT) &&
				(event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
			focus_toplevel(toplevel);
			if (event->button == BTN_LEFT) {
				begin_interactive(toplevel, WB_CURSOR_MOVE, 0);
			} else {
				/* Resize from the corner nearest the pointer. */
				uint32_t edges = 0;
				edges |= (cursor->x <
						toplevel->geometry.x + toplevel->geometry.width / 2.0)
						? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
				edges |= (cursor->y <
						toplevel->geometry.y + toplevel->geometry.height / 2.0)
						? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
				begin_interactive(toplevel, WB_CURSOR_RESIZE, edges);
			}
			wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
			return;
		}
	}

	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		focus_toplevel(toplevel);
	}
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
}

void wb_cursor::on_axis(void *data) {
	auto *event = static_cast<struct wlr_pointer_axis_event *>(data);
	wlr_seat_pointer_notify_axis(server->seat->seat, event->time_msec,
			event->orientation, event->delta, event->delta_discrete,
			event->source, event->relative_direction);
}

void wb_cursor::on_frame(void *) {
	wlr_seat_pointer_notify_frame(server->seat->seat);
}

void wb_cursor::on_request_set_cursor(void *data) {
	/* Raised when a client provides a cursor image. */
	auto *event = static_cast<struct wlr_seat_pointer_request_set_cursor_event *>(data);
	struct wlr_seat_client *focused_client =
		server->seat->seat->pointer_state.focused_client;
	/* Any client can send this, so only honour the one with pointer focus. */
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

wb_cursor::~wb_cursor() {
	/* Disconnect the listeners before destroying the wlr_cursor: it asserts
	 * that nothing is still listening on its event signals. (The Listener
	 * members would disconnect in their own destructors, but those run after
	 * this body, i.e. too late.) */
	cursor_motion.disconnect();
	cursor_motion_absolute.disconnect();
	cursor_button.disconnect();
	cursor_axis.disconnect();
	cursor_frame.disconnect();
	pointer_focus_change.disconnect();
	request_cursor.disconnect();

	if (xcursor_manager != nullptr) {
		wlr_xcursor_manager_destroy(xcursor_manager);
	}
	if (cursor != nullptr) {
		wlr_cursor_destroy(cursor);
	}
}

std::unique_ptr<wb_cursor> wb_cursor_create(struct wb_server *server) {
	auto cursor = std::make_unique<wb_cursor>();
	cursor->cursor = wlr_cursor_create();
	if (cursor->cursor == nullptr) {
		return nullptr;
	}
	cursor->server = server;

	const char *xcursor_size = getenv("XCURSOR_SIZE");
	cursor->xcursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
			xcursor_size ? strtoul(xcursor_size, nullptr, 10) : 24);

	wb_cursor *c = cursor.get();
	c->cursor_motion.connect(&c->cursor->events.motion,
			[c](void *data) { c->on_motion(data); });
	c->cursor_motion_absolute.connect(&c->cursor->events.motion_absolute,
			[c](void *data) { c->on_motion_absolute(data); });
	c->cursor_button.connect(&c->cursor->events.button,
			[c](void *data) { c->on_button(data); });
	c->cursor_axis.connect(&c->cursor->events.axis,
			[c](void *data) { c->on_axis(data); });
	c->cursor_frame.connect(&c->cursor->events.frame,
			[c](void *data) { c->on_frame(data); });
	c->pointer_focus_change.connect(
			&server->seat->seat->pointer_state.events.focus_change,
			[c](void *data) { c->on_pointer_focus_change(data); });
	c->request_cursor.connect(&server->seat->seat->events.request_set_cursor,
			[c](void *data) { c->on_request_set_cursor(data); });

	wlr_cursor_attach_output_layout(c->cursor, server->output_layout);
	return cursor;
}
