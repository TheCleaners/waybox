#include <time.h>
#include <cmath>
#include <optional>
#include <vector>

#include "idle.h"
#include "waybox/cursor.h"
#include "config.h"
#include "waybox/geometry.hpp"
#include "waybox/mousebind.hpp"
#include "waybox/output.h"
#include "waybox/placement.hpp"
#include "waybox/xdg_shell.h"
#include "waybox/menu_widget.hpp"
#include "waybox/frame_view.hpp"
#include "waybox/session_lock.h"
#include "waybox/pointer_constraints.h"

/* How close (in layout pixels) a dragged window's edge must come to a usable-
 * area or window edge before it snaps. */
static constexpr int WB_SNAP_DISTANCE = 16;

/* Run the action of titlebar button index `i` (iconify/maximize/close) for a
 * server-side-decorated toplevel. */
static void activate_frame_button(struct wb_toplevel *toplevel, int i) {
	if (toplevel == nullptr || !toplevel->frame)
		return;
	const std::vector<wb::FrameButton> &buttons = toplevel->frame->buttons();
	if (i < 0 || i >= static_cast<int>(buttons.size()))
		return;
	switch (buttons[i]) {
	case wb::FrameButton::Iconify:
		set_toplevel_minimized(toplevel, true);
		break;
	case wb::FrameButton::Maximize: {
		bool full = toplevel->max_horz && toplevel->max_vert;
		set_toplevel_maximized(toplevel, !full, !full);
		break;
	}
	case wb::FrameButton::Close:
		view_close(toplevel);
		break;
	case wb::FrameButton::Shade:
		set_toplevel_shaded(toplevel, !toplevel->shaded);
		break;
	case wb::FrameButton::AllDesktops:
		wb_toggle_omnipresent(toplevel);
		break;
	case wb::FrameButton::Icon:
		break;  /* acts on press (the window menu), not on release */
	}
}

static bool is_icon_button(struct wb_toplevel *t, int i) {
	return t->frame && i >= 0 && i < static_cast<int>(t->frame->buttons().size()) &&
			t->frame->buttons()[i] == wb::FrameButton::Icon;
}

/* Hide the edge-tiling preview and forget the zone. */
static void clear_tile_preview(struct wb_server *server) {
	server->pending_tile = static_cast<int>(wb::TileZone::None);
	server->pending_tile_output = nullptr;
	if (server->tile_preview != nullptr)
		wlr_scene_node_set_enabled(&server->tile_preview->node, false);
}

void reset_cursor_mode(struct wb_server *server) {
	/* Reset the cursor mode to passthrough */
	server->cursor->cursor_mode = WB_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
	clear_tile_preview(server);
}

/* While moving a window: find the edge zone under the pointer and show
 * where the window would go, just below the window being dragged. */
static void update_tile_preview(struct wb_server *server) {
	struct wb_config *config = server->config;
	struct wb_toplevel *toplevel = server->grabbed_toplevel;
	if (config == nullptr || !config->edge_tiling.enabled || toplevel == nullptr ||
			toplevel->fullscreen) {
		clear_tile_preview(server);
		return;
	}
	double cx = server->cursor->cursor->x, cy = server->cursor->cursor->y;
	struct wlr_output *output =
			wlr_output_layout_output_at(server->output_layout, cx, cy);
	wb::TileZone zone = wb::TileZone::None;
	if (output != nullptr) {
		std::vector<wb::Rect> outputs;
		wb::Rect here{};
		struct wb_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			struct wlr_box b;
			wlr_output_layout_get_box(server->output_layout, o->wlr_output, &b);
			if (wlr_box_empty(&b))
				continue;
			outputs.push_back(wb::Rect{b.x, b.y, b.width, b.height});
			if (o->wlr_output == output)
				here = outputs.back();
		}
		zone = wb::edge_zone(static_cast<int>(cx), static_cast<int>(cy), here,
				outputs, config->edge_tiling.range);
	}
	if (zone == wb::TileZone::None) {
		clear_tile_preview(server);
		return;
	}
	server->pending_tile = static_cast<int>(zone);
	server->pending_tile_output = output;

	wb::Rect r = tile_preview_rect(server, output, zone);
	if (server->tile_preview == nullptr) {
		/* The theme's active titlebar colour, translucent (premultiplied). */
		wb::Color c = config->theme.window_active.title_bg.color;
		const float a = 0.35f;
		const float rgba[4] = {c.r / 255.0f * a, c.g / 255.0f * a,
				c.b / 255.0f * a, a};
		server->tile_preview = wlr_scene_rect_create(server->toplevel_tree,
				r.width, r.height, rgba);
	}
	if (server->tile_preview == nullptr)
		return;
	wlr_scene_rect_set_size(server->tile_preview, r.width, r.height);
	wlr_scene_node_set_position(&server->tile_preview->node, r.x, r.y);
	if (toplevel->scene_tree->node.parent == server->toplevel_tree)
		wlr_scene_node_place_below(&server->tile_preview->node,
				&toplevel->scene_tree->node);
	wlr_scene_node_set_enabled(&server->tile_preview->node, true);
}

static bool run_mouse_bindings(struct wb_server *server, wb::MouseContext context,
		struct wb_toplevel *window, uint32_t button, uint32_t modifiers,
		wb::MouseEvent event);

static void process_cursor_move(struct wb_server *server) {
	/* Move the grabbed toplevel to the new position. */
	struct wb_toplevel *toplevel = server->grabbed_toplevel;
	/* Dragging a tiled or maximized window away gives it back its size,
	 * keeping the pointer at the same place along the titlebar; only once
	 * the pointer has really moved, so a click on the titlebar doesn't. */
	if ((toplevel->tiled || (toplevel->max_horz && toplevel->max_vert)) &&
			(std::abs(server->cursor->cursor->x - server->move_start_x) > 8 ||
			 std::abs(server->cursor->cursor->y - server->move_start_y) > 8)) {
		int old_w = toplevel->geometry.width;
		double along = old_w > 0 ? server->grab_x / old_w : 0.5;
		if (toplevel->tiled)
			untile_toplevel(toplevel, false);
		else
			set_toplevel_maximized(toplevel, false, false);
		server->grab_x = along * toplevel->geometry.width;
	}
	if (toplevel->scene_tree->node.type == WLR_SCENE_NODE_TREE) {
		toplevel->geometry.x = server->cursor->cursor->x - server->grab_x;
		toplevel->geometry.y = server->cursor->cursor->y - server->grab_y;

		/* Snap window/usable edges that come within range while dragging. */
		struct wlr_output *output = get_active_output(toplevel);
		if (output != nullptr && output->data != nullptr) {
			auto *wb_out = static_cast<struct wb_output *>(output->data);
			struct wlr_box ob;
			wlr_output_layout_get_box(server->output_layout, output, &ob);
			struct wlr_box ua = wb_out->usable_area;
			wb::Rect area{ob.x + ua.x, ob.y + ua.y, ua.width, ua.height};

			std::vector<wb::Rect> others;
			struct wb_toplevel *other;
			wl_list_for_each(other, &server->toplevels, link) {
				if (other == toplevel || !other->mapped ||
						!toplevel_on_current_desktop(other))
					continue;
				others.push_back(wb::Rect{other->geometry.x, other->geometry.y,
						other->geometry.width, other->geometry.height});
			}

			wb::Rect snapped = wb::snap_move(
					wb::Rect{toplevel->geometry.x, toplevel->geometry.y,
							toplevel->geometry.width, toplevel->geometry.height},
					area, others, WB_SNAP_DISTANCE);
			toplevel->geometry.x = snapped.x;
			toplevel->geometry.y = snapped.y;
		}

		/* Don't let the window be dragged over a panel's reserved area. */
		constrain_toplevel_to_usable_area(toplevel);
		position_toplevel(toplevel);
	}
	update_tile_preview(server);
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

	struct wlr_box geo_box = view_geometry(toplevel);

	/* Honor the client's size hints: clamp the proposed size to its min/max,
	 * keeping the non-dragged edges anchored. */
	wb::SizeHints hints{};
	view_size_hints(toplevel, &hints.min_width, &hints.min_height,
			&hints.max_width, &hints.max_height);
	wb::Rect resized = wb::clamp_resize(
			wb::Rect{new_left, new_top, new_right - new_left, new_bottom - new_top},
			(server->resize_edges & WLR_EDGE_LEFT) != 0,
			(server->resize_edges & WLR_EDGE_TOP) != 0,
			hints);

	toplevel->geometry.x = resized.x - geo_box.x;
	toplevel->geometry.y = resized.y - geo_box.y;
		position_toplevel(toplevel);

	view_set_size(toplevel, resized.width, resized.height);
}

static void process_cursor_motion(struct wb_server *server, uint32_t time) {
	/* Locked: pointer input only reaches the locker's surfaces. */
	if (session_locked(server)) {
		double sx, sy;
		struct wlr_seat *seat = server->seat->seat;
		struct wlr_surface *surface = session_lock_surface_at(server,
				server->cursor->cursor->x, server->cursor->cursor->y, &sx, &sy);
		if (surface != nullptr) {
			wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		} else {
			wlr_seat_pointer_clear_focus(seat);
			wlr_cursor_set_xcursor(server->cursor->cursor,
					server->cursor->xcursor_manager, "default");
		}
		idle_activity(server, seat);
		return;
	}

	/* A button held past the drag threshold: Drag bindings. */
	if (server->menu == nullptr) {
		std::optional<wb::ClickTracker::Pressed> held =
				server->click_tracker.pressed();
		if (held && !server->click_tracker.motion(server->cursor->cursor->x,
				server->cursor->cursor->y).empty()) {
			auto *window = reinterpret_cast<struct wb_toplevel *>(held->target);
			if (window == nullptr || toplevel_alive(server, window))
				run_mouse_bindings(server, held->context, window, held->button,
						held->modifiers, wb::MouseEvent::Drag);
		}
	}

		/* An open menu grabs the pointer: update its hover/submenu state and keep
	 * the default cursor; do not forward motion to clients. */
	if (server->menu != nullptr) {
		server->menu->on_motion(server->cursor->cursor->x,
				server->cursor->cursor->y);
		wlr_cursor_set_xcursor(server->cursor->cursor,
				server->cursor->xcursor_manager, "default");
		idle_activity(server,
				server->seat->seat);
		return;
	}

	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor->cursor_mode == WB_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor->cursor_mode == WB_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	/* Server-side decoration hover feedback: highlight the titlebar button under
	 * the pointer (if any), and show a resize cursor over the frame's border /
	 * corner grab areas. Cleared on every other frame. */
	{
		wb::FrameHit fh;
		struct wb_toplevel *fhover = toplevel_frame_at(server,
				server->cursor->cursor->x, server->cursor->cursor->y, &fh);
		int btn = (fhover != nullptr && fh.part == wb::FramePart::Button)
				? fh.button : -1;
		struct wb_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->frame)
				t->frame->set_hovered_button((t == fhover) ? btn : -1);
		}
		if (fhover != nullptr) {
			if (const char *cur = wb::frame_part_cursor(fh.part)) {
				wlr_cursor_set_xcursor(server->cursor->cursor,
						server->cursor->xcursor_manager, cur);
			} else {
				/* Titlebar / button: a normal pointer. */
				wlr_cursor_set_xcursor(server->cursor->cursor,
						server->cursor->xcursor_manager, "default");
			}
			/* The pointer is over our decoration, not the client surface: drop
			 * the client's pointer focus so that moving back inside the window
			 * re-sends a pointer enter, prompting the client to reassert its own
			 * cursor (otherwise it would keep the resize cursor we just set). */
			wlr_seat_pointer_clear_focus(server->seat->seat);
			idle_activity(server,
					server->seat->seat);
			return;
		}
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

	idle_activity(server, seat);
}


void cursor_refresh_focus(struct wb_server *server) {
	if (server->cursor == nullptr ||
			server->cursor->cursor_mode != WB_CURSOR_PASSTHROUGH)
		return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	process_cursor_motion(server,
			static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

void wb_cursor::on_motion(void *data) {
	auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
	if (wb_input_blocked(server, &event->pointer->base))
		return;
	double dx = event->delta_x, dy = event->delta_y;
	if (!pointer_constraints_motion(server, event->time_msec, &dx, &dy,
			event->unaccel_dx, event->unaccel_dy)) {
		/* Locked: the client reads raw motion; the cursor stays put. */
		idle_activity(server,
				server->seat->seat);
		return;
	}
	wlr_cursor_move(cursor, &event->pointer->base, dx, dy);
	process_cursor_motion(server, event->time_msec);
}

void wb_cursor::on_motion_absolute(void *data) {
	auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
	if (wb_input_blocked(server, &event->pointer->base))
		return;
	if (pointer_constraints_locked(server)) {
		idle_activity(server,
				server->seat->seat);
		return;
	}
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

/* The mouse-binding context under the pointer, and the window there (if
 * any). std::nullopt where bindings don't apply: a panel or other
 * layer-shell surface (its own clicks are its business) and the titlebar
 * buttons (which work on their own). */
static std::optional<wb::MouseContext> pointer_context(struct wb_server *server,
		struct wb_toplevel **window) {
	*window = nullptr;
	double x = server->cursor->cursor->x, y = server->cursor->cursor->y;
	wb::FrameHit fh;
	if (struct wb_toplevel *ft = toplevel_frame_at(server, x, y, &fh)) {
		*window = ft;
		switch (fh.part) {
		case wb::FramePart::Titlebar:     return wb::MouseContext::Titlebar;
		case wb::FramePart::BorderTop:    return wb::MouseContext::Top;
		case wb::FramePart::BorderBottom: return wb::MouseContext::Bottom;
		case wb::FramePart::BorderLeft:   return wb::MouseContext::Left;
		case wb::FramePart::BorderRight:  return wb::MouseContext::Right;
		case wb::FramePart::Button:
			/* The icon is a binding context (Openbox's Icon); the other
			 * buttons work on their own. */
			if (is_icon_button(ft, fh.button))
				return wb::MouseContext::Icon;
			return std::nullopt;
		default:                          return wb::MouseContext::Frame;
		}
	}
	double sx, sy;
	struct wlr_surface *surface = nullptr;
	struct wb_toplevel *toplevel = get_toplevel_at(server, x, y, &surface, &sx, &sy);
	if (toplevel != nullptr) {
		*window = toplevel;
		return wb::MouseContext::Client;
	}
	if (surface != nullptr)
		return std::nullopt;  /* a panel, a popup or an X11 menu */
	return wb::MouseContext::Root;
}

/* Run the mouse bindings for this event. A window binding focuses the
 * window first, so its actions act on the window that was clicked. */
static bool run_mouse_bindings(struct wb_server *server, wb::MouseContext context,
		struct wb_toplevel *window, uint32_t button, uint32_t modifiers,
		wb::MouseEvent event) {
	if (server->config == nullptr)
		return false;
	bool handled = false;
	/* An action may Reconfigure, freeing these bindings: stop then. */
	const struct wb_config *config = server->config;
	for (const wb::MouseBinding &binding : config->mouse_bindings) {
		if (!wb::mouse_binding_matches(binding, context, button, modifiers, event))
			continue;
		if (window != nullptr && !toplevel_alive(server, window))
			break;  /* closed by an earlier action */
		if (window != nullptr && !handled)
			focus_toplevel(window);
		/* Copy: an action may reconfigure, replacing the bindings. */
		std::vector<wb::Action> actions = binding.actions;
		server->action_window = window;
		for (const wb::Action &action : actions) {
			if (window != nullptr && !toplevel_alive(server, window))
				break;  /* closed by an earlier action */
			wb::run_action(action, server);
			if (server->config != config)
				break;  /* reconfigured: the rest belongs to the old config */
		}
		server->action_window = nullptr;
		handled = true;
		if (server->config != config)
			break;
	}
	return handled;
}

static uint32_t keyboard_modifiers(struct wb_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat->seat);
	return keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
}

void wb_cursor::on_button(void *data) {
	auto *event = static_cast<struct wlr_pointer_button_event *>(data);
	if (wb_input_blocked(server, &event->pointer->base))
		return;
	/* Release, Click and DoubleClick bindings run after everything else a
	 * release does (e.g. ending the move a titlebar press started). */
	std::optional<wb::ClickTracker::Pressed> pressed;
	std::vector<wb::MouseEvent> release_events;
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		pressed = server->click_tracker.pressed();
		release_events = server->click_tracker.release(event->button,
				event->time_msec);
	}
	handle_button(event);
	if (pressed && !session_locked(server) && server->menu == nullptr) {
		auto *window = reinterpret_cast<struct wb_toplevel *>(pressed->target);
		if (window != nullptr && !toplevel_alive(server, window))
			return;
		for (wb::MouseEvent e : release_events)
			run_mouse_bindings(server, pressed->context, window,
					pressed->button, pressed->modifiers, e);
	}
}

void wb_cursor::handle_button(struct wlr_pointer_button_event *event) {
	struct wlr_seat *seat = server->seat->seat;

	/* Locked: no bindings, frames or menus. The button goes to the pointer
	 * focus (only ever a lock surface), and a press on a lock surface gives it
	 * keyboard focus (e.g. the locker on another monitor). */
	if (session_locked(server)) {
		double sx, sy;
		struct wlr_surface *surface = session_lock_surface_at(server,
				cursor->x, cursor->y, &sx, &sy);
		if (surface != nullptr &&
				event->state == WL_POINTER_BUTTON_STATE_PRESSED)
			seat_focus_surface(server->seat.get(),
					wlr_surface_get_root_surface(surface));
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
				event->state);
		idle_activity(server, seat);
		return;
	}

	/* An open menu grabs the pointer: route the button to it and consume the
	 * event. Selecting an entry dismisses the menu and yields its actions, run
	 * only AFTER the widget is destroyed (no reentrancy). */
	if (server->menu != nullptr) {
		bool dismiss = server->menu->on_button(event->button,
				event->state == WL_POINTER_BUTTON_STATE_PRESSED);
		if (dismiss) {
			std::vector<wb::Action> actions = server->menu->take_actions();
			server->menu.reset();
			wb_run_menu_actions(server, std::move(actions));
		}
		idle_activity(server, seat);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = nullptr;
	struct wb_toplevel *toplevel = get_toplevel_at(server,
			cursor->x, cursor->y, &surface, &sx, &sy);

	/* Release of a held titlebar button: clear the pressed visual and, if the
	 * pointer is still over that same button, run its action (activate on
	 * release, like a normal push button). */
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED &&
			server->frame_pressed != nullptr) {
		struct wb_toplevel *pressed = server->frame_pressed;
		server->frame_pressed = nullptr;
		if (pressed->frame)
			pressed->frame->set_pressed_button(-1);
		wb::FrameHit rh;
		struct wb_toplevel *ft = toplevel_frame_at(server, cursor->x,
				cursor->y, &rh);
		if (ft == pressed && rh.part == wb::FramePart::Button)
			activate_frame_button(ft, rh.button);
		idle_activity(server, seat);
		return;
	}

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* Mouse bindings first: a Press binding takes the press over from
		 * the built-in behaviour below (titlebar drag, Alt+drag, ...). */
		struct wb_toplevel *bound_window = nullptr;
		if (auto context = pointer_context(server, &bound_window)) {
			uint32_t mods = keyboard_modifiers(server);
			server->click_tracker.press(event->button, *context,
					reinterpret_cast<uintptr_t>(bound_window), mods,
					cursor->x, cursor->y, event->time_msec);
			if (run_mouse_bindings(server, *context, bound_window,
					event->button, mods, wb::MouseEvent::Press) &&
					*context != wb::MouseContext::Client) {
				/* A binding took the press over. In a window (the Client
				 * context) the click still reaches the program, as in
				 * Openbox ("Focus, Raise" on a click must not eat it). */
				idle_activity(server, seat);
				return;
			}
		}

		/* Server-side decoration interaction: a press on a window's titlebar,
		 * border or a titlebar button. Resolved against the scene (respects
		 * z-order), so it takes priority over Root/Client bindings — a titlebar
		 * click must not open the root menu. */
		wb::FrameHit fh;
		if (struct wb_toplevel *ft = toplevel_frame_at(server, cursor->x,
				cursor->y, &fh)) {
			focus_toplevel(ft);
			if (fh.part == wb::FramePart::Titlebar) {
				if (event->button == wb::MOUSE_BUTTON_LEFT)
					begin_interactive(ft, WB_CURSOR_MOVE, 0);
			} else if (fh.part == wb::FramePart::Button && is_icon_button(ft, fh.button)) {
				/* The icon, without a binding for it: the window menu, as
				 * Openbox's default bindings do. */
				if (event->button == wb::MOUSE_BUTTON_LEFT ||
						event->button == wb::MOUSE_BUTTON_RIGHT) {
					server->action_window = ft;  /* not a modal dialog of it */
					wb_show_client_menu(server);
					server->action_window = nullptr;
				}
			} else if (fh.part == wb::FramePart::Button) {
				/* Show the pressed state now; the action runs on release. */
				if (event->button == wb::MOUSE_BUTTON_LEFT && ft->frame) {
					ft->frame->set_pressed_button(fh.button);
					server->frame_pressed = ft;
				}
			} else {
				uint32_t edges = static_cast<uint32_t>(
						wb::frame_part_resize_edges(fh.part));
				if (edges != 0 && event->button == wb::MOUSE_BUTTON_LEFT)
					begin_interactive(ft, WB_CURSOR_RESIZE, edges);
			}
			idle_activity(server, seat);
			return;
		}

		/* Alt + drag moves (left) or resizes (right) the window under the
		 * pointer regardless of its decorations, matching Openbox's default
		 * frame mouse bindings. The button is consumed, not forwarded. */
		uint32_t modifiers = keyboard_modifiers(server);
		if (toplevel != nullptr && (modifiers & WLR_MODIFIER_ALT) &&
				(event->button == wb::MOUSE_BUTTON_LEFT ||
						event->button == wb::MOUSE_BUTTON_RIGHT)) {
			focus_toplevel(toplevel);
			if (event->button == wb::MOUSE_BUTTON_LEFT) {
				begin_interactive(toplevel, WB_CURSOR_MOVE, 0);
			} else {
				/* Resize from the corner nearest the pointer. */
				begin_interactive(toplevel, WB_CURSOR_RESIZE,
						nearest_resize_edges(toplevel, cursor->x, cursor->y));
			}
			idle_activity(server, seat);
			return;
		}

	}

	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* Dropped on an edge zone: tile there. */
		if (server->cursor->cursor_mode == WB_CURSOR_MOVE &&
				server->grabbed_toplevel != nullptr &&
				server->pending_tile != static_cast<int>(wb::TileZone::None))
			tile_toplevel(server->grabbed_toplevel,
					static_cast<wb::TileZone>(server->pending_tile),
					server->pending_tile_output);
		reset_cursor_mode(server);
	} else {
		focus_toplevel(toplevel);
	}
	idle_activity(server, seat);
}

void wb_cursor::on_axis(void *data) {
	auto *event = static_cast<struct wlr_pointer_axis_event *>(data);
	if (wb_input_blocked(server, &event->pointer->base))
		return;
	/* A wheel notch is the "Up" or "Down" button: Press and Click bindings
	 * (scroll on the desktop to switch desktops, on a titlebar to shade).
	 * High-resolution wheels report fractions of a notch (value120), which
	 * add up; a wheel without notch information counts each event as one.
	 * Touchpad scrolling has no notches and always goes to clients. */
	int steps = event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL
			? wb::wheel_notches(&scroll_value120, event->delta_discrete,
					event->source == WL_POINTER_AXIS_SOURCE_WHEEL, event->delta)
			: 0;
	if (steps != 0 && !session_locked(server) && server->menu == nullptr &&
			server->cursor->cursor_mode == WB_CURSOR_PASSTHROUGH) {
		struct wb_toplevel *window = nullptr;
		if (auto context = pointer_context(server, &window)) {
			uint32_t button = steps < 0 ? wb::MOUSE_BUTTON_UP
					: wb::MOUSE_BUTTON_DOWN;
			uint32_t mods = keyboard_modifiers(server);
			bool pressed = run_mouse_bindings(server, *context, window, button,
					mods, wb::MouseEvent::Press);
			bool clicked = (window == nullptr || toplevel_alive(server, window)) &&
					run_mouse_bindings(server, *context, window, button, mods,
							wb::MouseEvent::Click);
			if (pressed || clicked)
				return;
		}
	}
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

void wb_cursor::on_request_set_shape(void *data) {
	/* A client requested a named cursor shape (cursor-shape-v1) instead of
	 * supplying a surface. Honour it only for the pointer-focused client. */
	auto *event =
		static_cast<struct wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
	struct wlr_seat_client *focused_client =
		server->seat->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_xcursor(cursor, xcursor_manager,
				wlr_cursor_shape_v1_name(event->shape));
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
	request_set_shape.disconnect();

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

	/* cursor-shape-v1: let clients request named cursor shapes. */
	struct wlr_cursor_shape_manager_v1 *cursor_shape =
		wlr_cursor_shape_manager_v1_create(server->wl_display, 1);
	c->request_set_shape.connect(&cursor_shape->events.request_set_shape,
			[c](void *data) { c->on_request_set_shape(data); });

	wlr_cursor_attach_output_layout(c->cursor, server->output_layout);
	return cursor;
}
