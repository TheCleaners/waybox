#include <time.h>
#include <cmath>
#include <optional>
#include <vector>

#include "toplevel_drag.h"
#include "idle.h"
#include "waybox/cursor.h"
#include "waybox/xcursor_themes.hpp"
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
#include "waybox/xwayland.h"
#include "color_pick.h"

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
	/* Just below the window, in its layer. */
	if (toplevel->scene_tree->node.parent == toplevel_layer_tree(server, toplevel->layer)) {
		wlr_scene_node_reparent(&server->tile_preview->node, toplevel->scene_tree->node.parent);
		wlr_scene_node_place_below(&server->tile_preview->node,
				&toplevel->scene_tree->node);
	}
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

namespace {
/* While a drag carries a window, it takes no part in finding what's under
 * the pointer (the drop target): hidden from the scene's hit tests for the
 * rest of the motion, shown again before anything is drawn. */
struct HideCarried {
	struct wb_server *server = nullptr;
	struct wb_toplevel *window = nullptr;
	explicit HideCarried(struct wb_toplevel *t) {
		if (t != nullptr && t->scene_tree != nullptr && t->scene_tree->node.enabled) {
			server = t->server;
			window = t;
			wlr_scene_node_set_enabled(&t->scene_tree->node, false);
		}
	}
	/* Shown again by the usual rule (a binding run meanwhile may have
	 * minimized it or switched desktops), if it's still there at all. */
	~HideCarried() {
		if (window != nullptr && toplevel_alive(server, window))
			toplevel_update_visibility(window);
	}
	HideCarried(const HideCarried &) = delete;
	HideCarried &operator=(const HideCarried &) = delete;
};
}  // namespace

static void process_cursor_motion(struct wb_server *server, uint32_t time) {
	/* Picking a color: the pointer points at nothing else. */
	if (color_pick_motion(server))
		return;
	if (server->seat->seat->drag != nullptr) {
		wlr_scene_node_set_position(&server->drag_icon_tree->node,
				static_cast<int>(server->cursor->cursor->x),
				static_cast<int>(server->cursor->cursor->y));
		toplevel_drag_motion(server);
	}
	HideCarried hide(server->seat->seat->drag != nullptr ?
			toplevel_drag_window(server) : nullptr);
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
			server->cursor->show_xcursor("default");
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
		server->cursor->show_xcursor("default");
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

	/* A button held on a client surface: it keeps the pointer (as in
	 * every compositor), even over other windows or a panel. Not during a
	 * drag-and-drop, which has to reach the window under the pointer. */
	{
		struct wlr_seat *seat = server->seat->seat;
		struct wb_cursor *c = server->cursor.get();
		if (c->grab_surface != nullptr && c->buttons_down > 0 &&
				seat->pointer_state.focused_surface == c->grab_surface &&
				seat->drag == nullptr) {
			wlr_seat_pointer_notify_motion(seat, time,
					c->cursor->x - c->grab_origin_x, c->cursor->y - c->grab_origin_y);
			idle_activity(server, seat);
			return;
		}
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
				server->cursor->show_xcursor(cur);
			} else {
				/* Titlebar / button: a normal pointer. */
				server->cursor->show_xcursor("default");
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
		server->cursor->show_xcursor("default");
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

/* ---- Touch ---------------------------------------------------------- */

void wb_cursor::touch_button(uint32_t time, bool pressed) {
	struct wlr_pointer_button_event ev = {};
	ev.time_msec = time;
	ev.button = wb::MOUSE_BUTTON_LEFT;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
	button_event(&ev);
	wlr_seat_pointer_notify_frame(server->seat->seat);
}

void wb_cursor::touch_cancel_all() {
	struct wlr_seat *seat = server->seat->seat;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t ms = static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	std::set<struct wlr_seat_client *> told;
	for (auto &[id, p] : touch_points) {
		if (p.emulated)
			continue;
		if (struct wlr_touch_point *point = wlr_seat_touch_get_point(seat, id))
			if (point->client != nullptr && told.insert(point->client).second)
				wlr_seat_touch_notify_cancel(seat, point->client);
	}
	bool emulated = emulating != -1;
	touch_points.clear();
	emulating = -1;
	if (emulated)
		touch_button(ms, false);
}

void cursor_touch_cancel_all(struct wb_server *server) {
	if (server->cursor != nullptr)
		server->cursor->touch_cancel_all();
}

void wb_cursor::init_touch() {
	touch_down.connect(&cursor->events.touch_down, [this](void *data) {
		auto *e = static_cast<struct wlr_touch_down_event *>(data);
		if (wb_input_blocked(server, &e->touch->base))
			return;
		struct wlr_seat *seat = server->seat->seat;
		double lx, ly, sx = 0, sy = 0;
		wlr_cursor_absolute_to_layout_coords(cursor, &e->touch->base, e->x, e->y, &lx, &ly);
		idle_activity(server, seat);
		struct wlr_surface *surface = nullptr;
		struct wb_toplevel *toplevel = nullptr;
		if (session_locked(server)) {
			surface = session_lock_surface_at(server, lx, ly, &sx, &sy);
		} else if (server->menu == nullptr && cursor_mode == WB_CURSOR_PASSTHROUGH) {
			toplevel = get_toplevel_at(server, lx, ly, &surface, &sx, &sy);
			/* A frame over the point: Waybox's, so a pointer. */
			wb::FrameHit fh;
			if (toplevel_frame_at(server, lx, ly, &fh) != nullptr)
				surface = nullptr;
		}
		if (surface != nullptr && wlr_surface_accepts_touch(surface, seat)) {
			if (toplevel != nullptr && !session_locked(server))
				focus_toplevel(toplevel);
			touch_points[e->touch_id] = {lx - sx, ly - sy, false};
			wlr_seat_touch_notify_down(seat, surface, e->time_msec, e->touch_id, sx, sy);
			return;
		}
		/* Waybox's own UI, or no touch support: the first finger is a
		 * pointer, pressing its left button where it touched. */
		if (emulating != -1)
			return;
		emulating = e->touch_id;
		touch_points[e->touch_id] = {0, 0, true};
		wlr_cursor_warp_closest(cursor, nullptr, lx, ly);
		process_cursor_motion(server, e->time_msec);
		touch_button(e->time_msec, true);
	});
	touch_motion.connect(&cursor->events.touch_motion, [this](void *data) {
		auto *e = static_cast<struct wlr_touch_motion_event *>(data);
		auto p = touch_points.find(e->touch_id);
		if (p == touch_points.end() || wb_input_blocked(server, &e->touch->base))
			return;
		double lx, ly;
		wlr_cursor_absolute_to_layout_coords(cursor, &e->touch->base, e->x, e->y, &lx, &ly);
		idle_activity(server, server->seat->seat);
		if (p->second.emulated) {
			wlr_cursor_warp_closest(cursor, nullptr, lx, ly);
			process_cursor_motion(server, e->time_msec);
			wlr_seat_pointer_notify_frame(server->seat->seat);
			return;
		}
		struct wlr_seat *seat = server->seat->seat;
		struct wlr_drag *drag = seat->drag;
		if (drag != nullptr && drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_TOUCH &&
				drag->touch_id == e->touch_id) {
			/* A drag-and-drop by this finger: the pointer, the drag icon and a
			 * carried window follow it, and the drop target is what's under
			 * it (the carried window left out). */
			wlr_cursor_warp_closest(cursor, nullptr, lx, ly);
			wlr_scene_node_set_position(&server->drag_icon_tree->node,
					static_cast<int>(lx), static_cast<int>(ly));
			toplevel_drag_motion(server);
			HideCarried hide(toplevel_drag_window(server));
			double sx, sy;
			struct wlr_surface *under = nullptr;
			get_toplevel_at(server, lx, ly, &under, &sx, &sy);
			if (under != nullptr) {
				p->second.off_x = lx - sx;
				p->second.off_y = ly - sy;
				wlr_seat_touch_point_focus(seat, under, e->time_msec, e->touch_id, sx, sy);
			} else {
				wlr_seat_touch_point_clear_focus(seat, e->time_msec, e->touch_id);
			}
		}
		wlr_seat_touch_notify_motion(seat, e->time_msec, e->touch_id,
				lx - p->second.off_x, ly - p->second.off_y);
	});
	touch_up.connect(&cursor->events.touch_up, [this](void *data) {
		auto *e = static_cast<struct wlr_touch_up_event *>(data);
		auto p = touch_points.find(e->touch_id);
		if (p == touch_points.end())
			return;
		bool emulated = p->second.emulated;
		touch_points.erase(p);
		idle_activity(server, server->seat->seat);
		if (emulated) {
			emulating = -1;
			touch_button(e->time_msec, false);
			return;
		}
		wlr_seat_touch_notify_up(server->seat->seat, e->time_msec, e->touch_id);
	});
	touch_cancel.connect(&cursor->events.touch_cancel, [this](void *data) {
		auto *e = static_cast<struct wlr_touch_cancel_event *>(data);
		auto p = touch_points.find(e->touch_id);
		if (p == touch_points.end())
			return;
		bool emulated = p->second.emulated;
		touch_points.erase(p);
		if (emulated) {
			emulating = -1;
			touch_button(e->time_msec, false);
			return;
		}
		if (struct wlr_touch_point *point =
				wlr_seat_touch_get_point(server->seat->seat, e->touch_id))
			wlr_seat_touch_notify_cancel(server->seat->seat, point->client);
	});
	touch_frame.connect(&cursor->events.touch_frame, [this](void *) {
		wlr_seat_touch_notify_frame(server->seat->seat);
	});
}

void wb_cursor::init_gestures() {
	gestures = wlr_pointer_gestures_v1_create(server->wl_display);
	struct wlr_seat *seat = server->seat->seat;
	/* Each one goes to the surface with pointer focus (wlroots sends it
	 * there), unless input from its device is blocked (locked screen). */
	auto pass = [this](struct wlr_pointer *pointer) {
		if (wb_input_blocked(server, &pointer->base))
			return false;
		idle_activity(server, server->seat->seat);
		return true;
	};
	swipe_begin.connect(&cursor->events.swipe_begin, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_swipe_begin_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_swipe_begin(gestures, seat, e->time_msec, e->fingers);
	});
	swipe_update.connect(&cursor->events.swipe_update, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_swipe_update_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_swipe_update(gestures, seat, e->time_msec, e->dx, e->dy);
	});
	swipe_end.connect(&cursor->events.swipe_end, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_swipe_end_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_swipe_end(gestures, seat, e->time_msec, e->cancelled);
	});
	pinch_begin.connect(&cursor->events.pinch_begin, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_pinch_begin_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_pinch_begin(gestures, seat, e->time_msec, e->fingers);
	});
	pinch_update.connect(&cursor->events.pinch_update, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_pinch_update_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_pinch_update(gestures, seat, e->time_msec,
					e->dx, e->dy, e->scale, e->rotation);
	});
	pinch_end.connect(&cursor->events.pinch_end, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_pinch_end_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_pinch_end(gestures, seat, e->time_msec, e->cancelled);
	});
	hold_begin.connect(&cursor->events.hold_begin, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_hold_begin_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_hold_begin(gestures, seat, e->time_msec, e->fingers);
	});
	hold_end.connect(&cursor->events.hold_end, [this, seat, pass](void *data) {
		auto *e = static_cast<struct wlr_pointer_hold_end_event *>(data);
		if (pass(e->pointer))
			wlr_pointer_gestures_v1_send_hold_end(gestures, seat, e->time_msec, e->cancelled);
	});
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
		show_xcursor("default");
	}
}

/* The mouse-binding context under the pointer, and the window there (if
 * any). Desktop on a surface in the background or bottom layer (a
 * desktop-icon program, or a wallpaper that takes input): Openbox's desktop
 * window. std::nullopt where bindings don't apply: a panel or other
 * layer-shell surface, popups (its own clicks are its business), and the
 * titlebar buttons (which work on their own). */
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
	if (surface != nullptr) {
		/* (A layer surface's popups are surfaces of their own: no layer.) */
		struct wlr_layer_surface_v1 *layer = wlr_layer_surface_v1_try_from_wlr_surface(
				wlr_surface_get_root_surface(surface));
		if (layer != nullptr &&
				(layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
				 layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM))
			return wb::MouseContext::Desktop;
		return std::nullopt;  /* a panel, a popup or an X11 menu */
	}
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
			if (action.type == wb::ActionType::Stop)
				break;  /* the rest of this binding's actions don't run */
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

/* Whether bindings that ran for a press (or scroll) keep it from the
 * surface under the pointer. In a window (Client) and on a desktop program
 * (Desktop) it still reaches the program, as in Openbox: "Focus, Raise" on
 * a click, or desktop switching on the wheel, must not eat it (Openbox's
 * own rc.xml binds both on Desktop). Except a binding that opened a menu on
 * a desktop program: the menu has the pointer now, and the program
 * mustn't open its own menu under it. Elsewhere (the empty desktop, the
 * frame) there's nothing to pass it to. */
static bool takes_click(struct wb_server *server, wb::MouseContext context, bool had_menu) {
	if (context == wb::MouseContext::Client)
		return false;
	if (context == wb::MouseContext::Desktop)
		return !had_menu && server->menu != nullptr;
	return true;
}

static uint32_t keyboard_modifiers(struct wb_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat->seat);
	return keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
}

void wb_cursor::on_button(void *data) {
	auto *event = static_cast<struct wlr_pointer_button_event *>(data);
	if (wb_input_blocked(server, &event->pointer->base))
		return;
	button_event(event);
}

void wb_cursor::button_event(struct wlr_pointer_button_event *event) {
	if (color_pick_button(server, event->button,
			event->state == WL_POINTER_BUTTON_STATE_PRESSED))
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
	/* A release owed to a program goes to it whatever Waybox does with it
	 * below; a later notify of the same release is then skipped. */
	bool owed = event->state == WL_POINTER_BUTTON_STATE_RELEASED &&
			client_buttons.erase(event->button) > 0;
	auto release_owed = [&]() {
		if (owed) {
			wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
					WL_POINTER_BUTTON_STATE_RELEASED);
			owed = false;
		}
	};

	/* Count every press and release, whatever path handles it below. */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		++buttons_down;
	} else if (buttons_down > 0 && --buttons_down == 0) {
		end_grab();
	}

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
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
			client_buttons.insert(event->button);
		else if (!owed)
			goto locked_done;  /* not ours to release */
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
				event->state);
	locked_done:
		idle_activity(server, seat);
		return;
	}

	/* An open menu grabs the pointer: route the button to it and consume the
	 * event. Selecting an entry dismisses the menu and yields its actions, run
	 * only AFTER the widget is destroyed (no reentrancy). */
	if (server->menu != nullptr) {
		release_owed();
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
	/* (Only the button that pressed it: the left one.) */
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED &&
			server->frame_pressed != nullptr && event->button == wb::MOUSE_BUTTON_LEFT) {
		release_owed();
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
			bool had_menu = server->menu != nullptr;
			if (run_mouse_bindings(server, *context, bound_window,
					event->button, mods, wb::MouseEvent::Press) &&
					takes_click(server, *context, had_menu)) {
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

	/* A release whose press went to Waybox (a frame, a binding, a menu):
	 * the program never saw the press, so it doesn't get the release
	 * either; what follows (ending a move) still happens. */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		client_buttons.insert(event->button);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED || owed)
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && this->buttons_down == 1 &&
			seat->pointer_state.focused_surface != nullptr) {
		/* The first button down: the surface under it keeps the pointer
		 * (until the last button is up, or it goes away). */
		this->end_grab();
		this->grab_surface = seat->pointer_state.focused_surface;
		this->grab_origin_x = cursor->x - seat->pointer_state.sx;
		this->grab_origin_y = cursor->y - seat->pointer_state.sy;
		this->grab_surface_destroy.connect(&this->grab_surface->events.destroy,
				[this](void *) { this->end_grab(); });
	}
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
			bool had_menu = server->menu != nullptr;
			bool pressed = run_mouse_bindings(server, *context, window, button,
					mods, wb::MouseEvent::Press);
			bool clicked = (window == nullptr || toplevel_alive(server, window)) &&
					run_mouse_bindings(server, *context, window, button, mods,
							wb::MouseEvent::Click);
			if ((pressed || clicked) && takes_click(server, *context, had_menu))
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
		xcursor_name.clear();  /* the program's own image */
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
		show_xcursor(wlr_cursor_shape_v1_name(event->shape));
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
	for (wb::Listener *l : {&swipe_begin, &swipe_update, &swipe_end, &pinch_begin,
			&pinch_update, &pinch_end, &hold_begin, &hold_end, &touch_down, &touch_up,
			&touch_motion, &touch_cancel, &touch_frame})
		l->disconnect();

	if (xcursor_manager != nullptr) {
		wlr_xcursor_manager_destroy(xcursor_manager);
	}
	if (cursor != nullptr) {
		wlr_cursor_destroy(cursor);
	}
}

void wb_cursor::show_xcursor(const char *name) {
	xcursor_name = name != nullptr ? name : "default";
	wlr_cursor_set_xcursor(cursor, xcursor_manager, xcursor_name.c_str());
}

void wb_cursor_apply_theme(struct wb_server *server) {
	wb_cursor *c = server->cursor.get();
	if (c == nullptr)
		return;
	const struct wb_config *config = server->config;
	std::string theme = config != nullptr && !config->cursor_theme.empty()
			? config->cursor_theme : c->start_theme;
	unsigned size = config != nullptr && config->cursor_size > 0
			? static_cast<unsigned>(config->cursor_size) : c->start_size;
	/* Programs started from now on use the same (Xwayland, startup
	 * programs, what scripts and menus run). */
	bool theme_from_rc = config != nullptr && !config->cursor_theme.empty();
	bool size_from_rc = config != nullptr && config->cursor_size > 0;
	/* A theme without any cursors (an empty "default" hiding the system's,
	 * left by some settings tool, say) would leave wlroots on its built-in
	 * cursor, which has one size: one that has some instead. */
	bool fell_back = false;
	{
		const char *home = getenv("HOME");
		std::vector<std::string> path = wb::xcursor_search_path(getenv("XCURSOR_PATH"),
				getenv("XDG_DATA_HOME"), home != nullptr ? home : "");
		wb::XcursorThemeCheck check = wb::xcursor_theme_check(theme, path);
		if (!check.has_cursors) {
			std::string instead = wb::xcursor_fallback_theme(path);
			std::string why = check.index_file.empty() ? std::string("not installed")
					: "its " + check.index_file + " falls back to nothing with cursors";
			if (theme != c->failed_theme)
				wlr_log(WLR_ERROR, "Cursor theme %s has no cursors (%s): using %s",
						theme.empty() ? "default" : theme.c_str(), why.c_str(),
						instead.empty() ? "the built-in cursor, at one size" : instead.c_str());
			c->failed_theme = theme;
			if (!instead.empty()) {
				theme = instead;
				fell_back = true;
			}
		} else {
			c->failed_theme.clear();
		}
	}
	if (theme_from_rc || c->start_theme_set || fell_back)
		setenv("XCURSOR_THEME", theme.c_str(), 1);
	else
		unsetenv("XCURSOR_THEME");
	if (size_from_rc || c->start_size_set)
		setenv("XCURSOR_SIZE", std::to_string(size).c_str(), 1);
	else
		unsetenv("XCURSOR_SIZE");
	if (theme == c->theme && size == c->size)
		return;
	struct wlr_xcursor_manager *fresh = wlr_xcursor_manager_create(
			theme.empty() ? nullptr : theme.c_str(), size);
	if (fresh == nullptr) {
		wlr_log(WLR_ERROR, "Could not load the cursor theme %s at size %u", theme.c_str(), size);
		return;
	}
	/* The new one in use (and the cursor shown from it) before the old
	 * one goes: the cursor keeps the manager it was last shown from. */
	struct wlr_xcursor_manager *old = c->xcursor_manager;
	c->xcursor_manager = fresh;
	c->theme = theme;
	c->size = size;
	if (!c->xcursor_name.empty())
		wlr_cursor_set_xcursor(c->cursor, fresh, c->xcursor_name.c_str());
	wlr_xcursor_manager_destroy(old);
	xwayland_update_cursor(server);
	wlr_log(WLR_INFO, "wb-cursor theme %s size %u", theme.empty() ? "(default)" : theme.c_str(),
			size);
}

std::unique_ptr<wb_cursor> wb_cursor_create(struct wb_server *server) {
	auto cursor = std::make_unique<wb_cursor>();
	cursor->cursor = wlr_cursor_create();
	if (cursor->cursor == nullptr) {
		return nullptr;
	}
	cursor->server = server;

	/* What Waybox was started with, until (and unless) rc.xml says. */
	if (const char *t = getenv("XCURSOR_THEME"); t != nullptr && *t) {
		cursor->start_theme = t;
		cursor->start_theme_set = true;
	}
	if (const char *sz = getenv("XCURSOR_SIZE"); sz != nullptr && *sz) {
		unsigned long v = strtoul(sz, nullptr, 10);
		if (v >= 8 && v <= 512) {
			cursor->start_size = static_cast<unsigned>(v);
			cursor->start_size_set = true;
		}
	}
	cursor->theme = cursor->start_theme;
	cursor->size = cursor->start_size;
	cursor->xcursor_manager = wlr_xcursor_manager_create(
			cursor->theme.empty() ? nullptr : cursor->theme.c_str(), cursor->size);

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

	c->init_gestures();
	c->init_touch();

	wlr_cursor_attach_output_layout(c->cursor, server->output_layout);
	return cursor;
}
