#include "waybox/pointer_constraints.h"

#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/seat.h"
#include "waybox/server.h"
#include "waybox/session_lock.h"

namespace {

/* Per-constraint listeners, freed with the constraint. */
struct Constraint {
	struct wb_server *server = nullptr;
	struct wlr_pointer_constraint_v1 *constraint = nullptr;
	wb::Listener set_region;
	wb::Listener destroy;
};

/* Where the pointer's focused surface's origin is in layout coordinates
 * (the cursor minus its surface-local position). */
void surface_origin(struct wb_server *server, double *x, double *y) {
	struct wlr_seat *seat = server->seat->seat;
	*x = server->cursor->cursor->x - seat->pointer_state.sx;
	*y = server->cursor->cursor->y - seat->pointer_state.sy;
}

/* A locked pointer may say where the cursor should appear once unlocked
 * (e.g. where a game drew its own cursor); honour it. */
void warp_to_hint(struct wb_server *server, struct wlr_pointer_constraint_v1 *c) {
	if (c->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
			!c->current.cursor_hint.enabled ||
			server->seat->seat->pointer_state.focused_surface != c->surface)
		return;
	double ox, oy;
	surface_origin(server, &ox, &oy);
	wlr_cursor_warp(server->cursor->cursor, nullptr,
			ox + c->current.cursor_hint.x, oy + c->current.cursor_hint.y);
	wlr_seat_pointer_warp(server->seat->seat, c->current.cursor_hint.x,
			c->current.cursor_hint.y);
}

void set_active(struct wb_server *server, struct wlr_pointer_constraint_v1 *c) {
	struct wlr_pointer_constraint_v1 *old = server->active_constraint;
	if (old == c)
		return;
	if (old != nullptr) {
		warp_to_hint(server, old);
		wlr_pointer_constraint_v1_send_deactivated(old);
		/* A one-shot constraint is destroyed by that; our destroy handler
		 * has already cleared active_constraint in that case. */
	}
	server->active_constraint = c;
	if (c != nullptr) {
		wlr_log(WLR_DEBUG, "pointer %s",
				c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "locked" : "confined");
		wlr_pointer_constraint_v1_send_activated(c);
	}
}

void new_constraint(struct wb_server *server, void *data) {
	auto *c = static_cast<struct wlr_pointer_constraint_v1 *>(data);
	auto *wc = new Constraint{};
	wc->server = server;
	wc->constraint = c;
	wc->set_region.connect(&c->events.set_region, [](void *) {
		/* The region is read from the constraint on every motion. */
	});
	wc->destroy.connect(&c->events.destroy, [wc](void *) {
		struct wb_server *server = wc->server;
		if (server->active_constraint == wc->constraint) {
			warp_to_hint(server, wc->constraint);
			server->active_constraint = nullptr;
		}
		delete wc;  /* the Listener members disconnect themselves */
	});
	pointer_constraints_update(server);
}

/* Constraints act only on ordinary pointer input: not while the session is
 * locked, a menu has the pointer, or a window is being moved/resized. */
bool constraints_apply(struct wb_server *server) {
	return server->active_constraint != nullptr && !session_locked(server) &&
			server->menu == nullptr &&
			server->cursor->cursor_mode == WB_CURSOR_PASSTHROUGH;
}

}  // namespace

void init_pointer_constraints(struct wb_server *server) {
	server->relative_pointer_manager =
			wlr_relative_pointer_manager_v1_create(server->wl_display);
	server->pointer_constraints =
			wlr_pointer_constraints_v1_create(server->wl_display);
	server->new_pointer_constraint.connect(
			&server->pointer_constraints->events.new_constraint,
			[server](void *data) { new_constraint(server, data); });
	struct wlr_seat *seat = server->seat->seat;
	server->constraint_keyboard_focus.connect(
			&seat->keyboard_state.events.focus_change,
			[server](void *) { pointer_constraints_update(server); });
	server->constraint_pointer_focus.connect(
			&seat->pointer_state.events.focus_change,
			[server](void *) { pointer_constraints_update(server); });
}

void finish_pointer_constraints(struct wb_server *server) {
	server->new_pointer_constraint.disconnect();
	server->constraint_keyboard_focus.disconnect();
	server->constraint_pointer_focus.disconnect();
	server->active_constraint = nullptr;
}

void pointer_constraints_update(struct wb_server *server) {
	if (server->pointer_constraints == nullptr)
		return;
	struct wlr_seat *seat = server->seat->seat;
	struct wlr_surface *focused = seat->keyboard_state.focused_surface;
	struct wlr_pointer_constraint_v1 *c = nullptr;
	if (focused != nullptr && seat->pointer_state.focused_surface == focused)
		c = wlr_pointer_constraints_v1_constraint_for_surface(
				server->pointer_constraints, focused, seat);
	set_active(server, c);
}

bool pointer_constraints_motion(struct wb_server *server, uint32_t time_msec,
		double *dx, double *dy, double unaccel_dx, double unaccel_dy) {
	/* Raw motion goes to the client with pointer focus whether or not it
	 * constrains the pointer (it may just want unaccelerated motion), but
	 * not while the compositor itself has the pointer. */
	if (server->relative_pointer_manager != nullptr && !session_locked(server) &&
			server->menu == nullptr)
		wlr_relative_pointer_manager_v1_send_relative_motion(
				server->relative_pointer_manager, server->seat->seat,
				static_cast<uint64_t>(time_msec) * 1000, *dx, *dy,
				unaccel_dx, unaccel_dy);

	if (!constraints_apply(server))
		return true;
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return false;

	/* Confined: clip the motion to the region (surface-local). */
	struct wlr_seat *seat = server->seat->seat;
	double sx = seat->pointer_state.sx, sy = seat->pointer_state.sy;
	double nx, ny;
	if (wlr_region_confine(&c->region, sx, sy, sx + *dx, sy + *dy, &nx, &ny)) {
		*dx = nx - sx;
		*dy = ny - sy;
	}
	return true;
}

bool pointer_constraints_locked(struct wb_server *server) {
	return constraints_apply(server) &&
			server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}
