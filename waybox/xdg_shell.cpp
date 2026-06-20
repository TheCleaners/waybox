#include "waybox/wlroots.hpp"

#include "idle.h"
#include "waybox/geometry.hpp"
#include "waybox/xdg_shell.h"

static void log_geometry(struct wb_toplevel *toplevel, const char *tag);

struct wb_toplevel *get_toplevel_at(
		struct wb_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * we only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a wb_toplevel. */
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Walk up to the nearest node carrying a data pointer. Both toplevels and
	 * layer-shell surfaces/popups set node.data, but to different types, so we
	 * must confirm the result is actually one of our toplevels before
	 * returning it -- otherwise clicking a panel (e.g. Waybar) would hand back
	 * a wb_scene_descriptor reinterpreted as a wb_toplevel. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	struct wb_toplevel *candidate = static_cast<struct wb_toplevel *>(tree->node.data);
	struct wb_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel == candidate) {
			return candidate;
		}
	}
	return NULL;
}

/* Returns the active (most recently focused) toplevel, or NULL if there are
 * none. This is the head of the focus order, which is independent of the
 * stacking order (server->toplevels). */
struct wb_toplevel *first_toplevel(struct wb_server *server) {
	if (wl_list_empty(&server->focus_order)) {
		return NULL;
	}
	struct wb_toplevel *toplevel =
		wl_container_of(server->focus_order.next, toplevel, focus_link);
	return toplevel;
}

/* Raise a toplevel to the top of the stacking order (z-order), without
 * touching focus. */
void raise_toplevel(struct wb_toplevel *toplevel) {
	if (toplevel == NULL)
		return;
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
}

/* Lower a toplevel to the bottom of the stacking order, without touching
 * focus. */
void lower_toplevel(struct wb_toplevel *toplevel) {
	if (toplevel == NULL)
		return;
	wlr_scene_node_lower_to_bottom(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(toplevel->server->toplevels.prev, &toplevel->link);
}

void focus_toplevel(struct wb_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus (plus the stacking
	 * raise that conventionally accompanies it). */
	if (toplevel == NULL || toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}
	/* Never activate a surface that has not completed its initial commit:
	 * wlr_xdg_toplevel_set_activated() schedules a configure, which asserts on
	 * an uninitialized surface. Toplevels live in the focus order from creation
	 * (before they map), so cycling/refocus can reach one this early. */
	if (!toplevel->xdg_toplevel->base->initialized) {
		return;
	}

	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
	if (xdg_surface != NULL && xdg_surface->toplevel != NULL)
		wlr_log(WLR_INFO, "%s: %s", _("Keyboard focus is now on surface"),
				xdg_surface->toplevel->app_id ? xdg_surface->toplevel->app_id : "(unnamed)");

	struct wb_server *server = toplevel->server;
	if (server->seat->focused_layer != NULL) {
		/* If a layer is focused, don't focus a toplevel. */
		return;
	}
	struct wlr_seat *seat = server->seat->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Already focused: still promote it in the focus order and raise it,
		 * so repeated focus requests keep the MRU/stacking invariants. */
		wl_list_remove(&toplevel->focus_link);
		wl_list_insert(&server->focus_order, &toplevel->focus_link);
		raise_toplevel(toplevel);
		return;
	}
	if (prev_surface != NULL) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	/* Promote to the head of the focus order and raise to the top of the
	 * stacking order (focusing conventionally raises). */
	wl_list_remove(&toplevel->focus_link);
	wl_list_insert(&server->focus_order, &toplevel->focus_link);
	raise_toplevel(toplevel);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	seat_focus_surface(server->seat.get(), surface);
}

struct wlr_output *get_active_output(struct wb_toplevel *toplevel) {
	double closest_x, closest_y;
	struct wlr_output *output = NULL;
	wlr_output_layout_closest_point(toplevel->server->output_layout, output,
			toplevel->geometry.x + toplevel->geometry.width / 2,
			toplevel->geometry.y + toplevel->geometry.height / 2,
			&closest_x, &closest_y);
	return wlr_output_layout_output_at(toplevel->server->output_layout, closest_x, closest_y);
}

/* Focus the most-recently-focused mapped toplevel other than `except`, e.g.
 * after the active window is unmapped or minimised. Walks the MRU focus order. */
static void focus_next_after(struct wb_server *server, struct wb_toplevel *except) {
	struct wb_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->focus_order, focus_link) {
		if (toplevel == except)
			continue;
		if (toplevel->mapped) {
			focus_toplevel(toplevel);
			return;
		}
	}
}

static struct wlr_box get_usable_area(struct wb_toplevel *toplevel) {
	struct wlr_output *output = get_active_output(toplevel);
	struct wlr_box usable_area = {};
	if (output != NULL) {
		struct wb_output *wb_output = static_cast<struct wb_output *>(output->data);
		if (wb_output != NULL && wb_output->usable_area.width > 0 &&
				wb_output->usable_area.height > 0) {
			/* Area left after subtracting layer-shell exclusive zones. */
			usable_area = wb_output->usable_area;
		} else {
			wlr_output_effective_resolution(output,
					&usable_area.width, &usable_area.height);
		}
	}
	return usable_area;
}

/* Re-apply geometry to managed toplevels after the output configuration
 * changes (mode, scale, transform, or panel reservation). Maximized and
 * fullscreen windows are resized to match, and floating windows are clamped
 * so they remain reachable. */
void arrange_toplevels(struct wb_server *server) {
	struct wb_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->xdg_toplevel == NULL || toplevel->scene_tree == NULL ||
				!toplevel->xdg_toplevel->base->initialized) {
			continue;
		}

		if (toplevel->xdg_toplevel->current.fullscreen) {
			struct wlr_output *output = get_active_output(toplevel);
			if (output == NULL)
				continue;
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output, &box);
			if (wlr_box_empty(&box))
				continue;
			toplevel->geometry = box;
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
					box.width, box.height);
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
					box.x, box.y);
			continue;
		}

		if (toplevel->max_horz || toplevel->max_vert) {
			/* Re-apply the (possibly partial) maximized geometry against the
			 * new usable area; this does not re-snapshot the restore rect. */
			set_toplevel_maximized(toplevel, toplevel->max_horz,
					toplevel->max_vert);
			continue;
		}

		struct wlr_box usable = get_usable_area(toplevel);
		if (usable.width <= 0 || usable.height <= 0)
			continue;

		/* Keep floating windows reachable after a resolution/scale change. */
		int max_x = usable.x + usable.width - 1;
		int max_y = usable.y + usable.height - 1;
		if (toplevel->geometry.x > max_x)
			toplevel->geometry.x = max_x;
		if (toplevel->geometry.y > max_y)
			toplevel->geometry.y = max_y;
		if (toplevel->geometry.x < usable.x)
			toplevel->geometry.x = usable.x;
		if (toplevel->geometry.y < usable.y)
			toplevel->geometry.y = usable.y;
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
				toplevel->geometry.x, toplevel->geometry.y);
	}
}

/* Clamp a toplevel's position so it does not cover layer-shell reserved areas
 * (e.g. a panel like Waybar). Only edges that actually have a reservation
 * constrain the window, so it can still be dragged off-screen on free edges. */
void constrain_toplevel_to_usable_area(struct wb_toplevel *toplevel) {
	struct wlr_output *output = get_active_output(toplevel);
	if (output == NULL || output->data == NULL)
		return;
	struct wb_output *wb_output = static_cast<struct wb_output *>(output->data);
	struct wlr_box usable = wb_output->usable_area;
	if (usable.width <= 0 || usable.height <= 0)
		return;

	struct wlr_box output_box;
	wlr_output_layout_get_box(toplevel->server->output_layout, output,
			&output_box);
	if (wlr_box_empty(&output_box))
		return;

	/* usable_area is output-local; shift it into layout coordinates so it shares
	 * a space with the toplevel geometry and the output box. */
	wb::Rect box{toplevel->geometry.x, toplevel->geometry.y,
			toplevel->geometry.width, toplevel->geometry.height};
	wb::Rect outer{output_box.x, output_box.y, output_box.width,
			output_box.height};
	wb::Rect usable_layout{output_box.x + usable.x, output_box.y + usable.y,
			usable.width, usable.height};

	wb::Rect constrained = wb::constrain_to_usable(box, outer, usable_layout);
	toplevel->geometry.x = constrained.x;
	toplevel->geometry.y = constrained.y;
}

static void xdg_toplevel_map(struct wb_toplevel *toplevel, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	if (toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;

	toplevel->mapped = true;

	struct wb_config *config = toplevel->server->config;
	struct wlr_box usable_area = get_usable_area(toplevel);
	struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;

	/* Placement region: the usable area inset by the configured margins. */
	wb::Rect area{usable_area.x, usable_area.y, usable_area.width,
			usable_area.height};
	if (config)
		area = wb::apply_strut(area, wb::Strut{config->margins.left,
				config->margins.top, config->margins.right,
				config->margins.bottom});

	int width = MIN(geo_box.width, area.width);
	int height = MIN(geo_box.height, area.height);

	/* Gather the other mapped windows so Smart placement can avoid them. */
	std::vector<wb::Rect> others;
	struct wb_toplevel *other;
	wl_list_for_each(other, &toplevel->server->toplevels, link) {
		if (other == toplevel || !other->mapped)
			continue;
		others.push_back(wb::Rect{other->geometry.x, other->geometry.y,
				other->geometry.width, other->geometry.height});
	}

	wb::PlacementPolicy policy =
			config ? config->placement_policy : wb::PlacementPolicy::Smart;
	wb::Rect placed;
	switch (policy) {
	case wb::PlacementPolicy::Center:
		placed = wb::place_center(area, width, height);
		break;
	case wb::PlacementPolicy::UnderMouse:
		placed = wb::place_under_mouse(area, width, height,
				static_cast<int>(toplevel->server->cursor->cursor->x),
				static_cast<int>(toplevel->server->cursor->cursor->y));
		break;
	case wb::PlacementPolicy::Smart:
	default:
		placed = wb::place_smart(area, width, height, others);
		break;
	}

	/* Per-application rules can override position/size and request initial
	 * window states. Position overrides are relative to the placement area. */
	const wb::AppRule *rule = nullptr;
	if (config) {
		const char *app_id = toplevel->xdg_toplevel->app_id
				? toplevel->xdg_toplevel->app_id : "";
		const char *title = toplevel->xdg_toplevel->title
				? toplevel->xdg_toplevel->title : "";
		rule = wb::match_app_rule(config->app_rules, app_id, title);
	}
	if (rule != nullptr) {
		if (rule->width)
			placed.width = *rule->width;
		if (rule->height)
			placed.height = *rule->height;
		if (rule->x)
			placed.x = area.x + *rule->x;
		if (rule->y)
			placed.y = area.y + *rule->y;
	}

	toplevel->geometry = {placed.x, placed.y, placed.width, placed.height};

	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->geometry.width, toplevel->geometry.height);

	bool do_focus = !(rule && rule->focus.has_value() && !*rule->focus);
	if (do_focus)
		focus_toplevel(toplevel);

	wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->geometry.x, toplevel->geometry.y);
	log_geometry(toplevel, "mapped");

	/* Apply requested initial states after the base mapping. */
	if (rule != nullptr) {
		if (rule->maximized.value_or(false))
			set_toplevel_maximized(toplevel, true, true);
		if (rule->fullscreen.value_or(false))
			set_toplevel_fullscreen(toplevel, true);
		if (rule->iconic.value_or(false)) {
			toplevel->xdg_toplevel->requested.minimized = true;
			wl_signal_emit(&toplevel->xdg_toplevel->events.request_minimize, NULL);
		}
	}
}

static void xdg_toplevel_unmap(struct wb_toplevel *toplevel, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	if (toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;
	toplevel->mapped = false;
	reset_cursor_mode(toplevel->server);

	/* Focus the next most-recently-used toplevel, if any. */
	focus_next_after(toplevel->server, toplevel);
}

static void update_fractional_scale(struct wlr_surface *surface) {
	float scale = 1;
	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (surface_output->output->scale > scale) {
			scale = surface_output->output->scale;
		}
	}
	wlr_fractional_scale_v1_notify_scale(surface, scale);
	wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
}

static void xdg_toplevel_commit(struct wb_toplevel *toplevel, void *data) {
	struct wlr_xdg_surface *base = toplevel->xdg_toplevel->base;

	struct wlr_output *output = get_active_output(toplevel);
	if (output != NULL)
		wlr_surface_send_enter(base->surface, output);
	update_fractional_scale(base->surface);
	if (base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		if (toplevel->decoration != NULL)
			wl_signal_emit(&toplevel->decoration->events.request_mode, toplevel->decoration);
	}
}

static void xdg_toplevel_destroy(struct wb_toplevel *toplevel, void *data) {
	/* Called when the xdg_toplevel is destroyed and should never be shown again. */

	struct wlr_output *output = get_active_output(toplevel);
	struct wlr_xdg_surface *base = toplevel->xdg_toplevel->base;
	if (output != NULL)
		wlr_surface_send_leave(base->surface, output);
	update_fractional_scale(base->surface);
	wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->foreign_toplevel_handle);

	/* The wb::Listener members disconnect themselves when the toplevel is
	 * deleted. */
	wl_list_remove(&toplevel->link);
	wl_list_remove(&toplevel->focus_link);
	delete toplevel;
}

static void xdg_toplevel_set_app_id(
		struct wb_toplevel *toplevel, void *data) {
	toplevel->foreign_toplevel_state.app_id = toplevel->xdg_toplevel->app_id;
	wlr_ext_foreign_toplevel_handle_v1_update_state(
			toplevel->foreign_toplevel_handle, &toplevel->foreign_toplevel_state);
}

static void xdg_toplevel_set_title(
		struct wb_toplevel *toplevel, void *data) {
	toplevel->foreign_toplevel_state.title = toplevel->xdg_toplevel->title;
	wlr_ext_foreign_toplevel_handle_v1_update_state(
			toplevel->foreign_toplevel_handle, &toplevel->foreign_toplevel_state);
}

/* Log a toplevel's geometry on a state transition. Helps debugging window
 * placement, and lets the headless integration test verify restore geometry. */
static void log_geometry(struct wb_toplevel *toplevel, const char *tag) {
	wlr_log(WLR_INFO, "wb-geom %s %dx%d+%d+%d", tag,
			toplevel->geometry.width, toplevel->geometry.height,
			toplevel->geometry.x, toplevel->geometry.y);
}

/* Margin-inset usable area for `toplevel`, as a wb::Rect in layout coords. */
static wb::Rect maximize_area(struct wb_toplevel *toplevel) {
	struct wlr_box usable = get_usable_area(toplevel);
	wb::Rect area{usable.x, usable.y, usable.width, usable.height};
	struct wb_config *config = toplevel->server->config;
	if (config) {
		area = wb::apply_strut(area,
				wb::Strut{config->margins.left, config->margins.top,
						config->margins.right, config->margins.bottom});
	}
	return area;
}

void set_toplevel_maximized(struct wb_toplevel *toplevel, bool horz, bool vert) {
	if (!toplevel->xdg_toplevel->base->initialized)
		return;

	const bool was_any = toplevel->max_horz || toplevel->max_vert;
	const bool now_any = horz || vert;
	/* Snapshot the floating geometry the first time we maximize any axis. */
	if (!was_any && now_any)
		toplevel->restore_maximize = toplevel->geometry;

	toplevel->max_horz = horz;
	toplevel->max_vert = vert;

	wb::Rect restore{toplevel->restore_maximize.x, toplevel->restore_maximize.y,
			toplevel->restore_maximize.width, toplevel->restore_maximize.height};
	wb::Rect g = wb::maximize_within(restore, maximize_area(toplevel), horz, vert);
	toplevel->geometry = {g.x, g.y, g.width, g.height};

	if (now_any)
		raise_toplevel(toplevel);

	/* The xdg protocol only has a single "maximized" state, so only claim it
	 * when fully maximized; partial maximize is just a managed resize. */
	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, horz && vert);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, g.width, g.height);
	wlr_scene_node_set_position(&toplevel->scene_tree->node, g.x, g.y);
	log_geometry(toplevel, now_any ? "maximize-on" : "maximize-off");
}

void set_toplevel_fullscreen(struct wb_toplevel *toplevel, bool fullscreen) {
	if (!toplevel->xdg_toplevel->base->initialized)
		return;

	if (fullscreen) {
		toplevel->restore_fullscreen = toplevel->geometry;
		struct wlr_output *wlr_output = get_active_output(toplevel);
		struct wb_output *output = static_cast<struct wb_output *>(
				wlr_output ? wlr_output->data : NULL);
		if (output != NULL)
			toplevel->geometry = output->geometry;  /* layout box: pos + size */
		raise_toplevel(toplevel);
	} else {
		toplevel->geometry = toplevel->restore_fullscreen;
	}

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->geometry.width, toplevel->geometry.height);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->geometry.x, toplevel->geometry.y);
	log_geometry(toplevel, fullscreen ? "fullscreen-on" : "fullscreen-off");
}

static void xdg_toplevel_request_fullscreen(
		struct wb_toplevel *toplevel, void *data) {
	/* Toggle fullscreen on the client's request (e.g. a video player). A
	 * request before the first commit would assert in wlroots, so it is
	 * ignored until the surface is initialized; the client can re-request. */
	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	set_toplevel_fullscreen(toplevel,
			!toplevel->xdg_toplevel->current.fullscreen);
}

static void xdg_toplevel_request_maximize(struct wb_toplevel *toplevel, void *data) {
	/* The client asked to (un)maximize, e.g. via its CSD maximize button.
	 * Toggle full maximize. Ignored until initialized (see above). */
	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	const bool full = toplevel->max_horz && toplevel->max_vert;
	set_toplevel_maximized(toplevel, !full, !full);
}

static void xdg_toplevel_request_minimize(struct wb_toplevel *toplevel, void *data) {
	bool minimize_requested = toplevel->xdg_toplevel->requested.minimized;
	if (minimize_requested) {
		toplevel->restore_minimize = toplevel->geometry;
		toplevel->geometry.y = -toplevel->geometry.height;

		focus_next_after(toplevel->server, toplevel);
	} else {
		toplevel->geometry = toplevel->restore_minimize;
	}

	wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->geometry.x, toplevel->geometry.y);
}

void begin_interactive(struct wb_toplevel *toplevel,
		enum wb_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propagating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct wb_server *server = toplevel->server;
	server->grabbed_toplevel = toplevel;
	server->cursor->cursor_mode = mode;

	if (mode == WB_CURSOR_MOVE) {
		server->grab_x = server->cursor->cursor->x - toplevel->geometry.x;
		server->grab_y = server->cursor->cursor->y - toplevel->geometry.y;
	} else if (mode == WB_CURSOR_RESIZE) {
		struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;

		double border_x = (toplevel->geometry.x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (toplevel->geometry.y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->cursor->x - border_x;
		server->grab_y = server->cursor->cursor->y - border_y;

		server->grab_geo_box = geo_box;
		server->grab_geo_box.x += toplevel->geometry.x;
		server->grab_geo_box.y += toplevel->geometry.y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wb_toplevel *toplevel, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. */
	begin_interactive(toplevel, WB_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wb_toplevel *toplevel, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. */
	struct wlr_xdg_toplevel_resize_event *event = static_cast<struct wlr_xdg_toplevel_resize_event *>(data);
	begin_interactive(toplevel, WB_CURSOR_RESIZE, event->edges);
}

/* Walk a popup's parent chain to the toplevel it ultimately belongs to, or
 * NULL if it isn't rooted in an xdg toplevel. */
static struct wb_toplevel *popup_root_toplevel(struct wlr_xdg_popup *xdg_popup) {
	struct wlr_surface *parent = xdg_popup->parent;
	while (parent != NULL) {
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_try_from_wlr_surface(parent);
		if (xdg_surface == NULL)
			return NULL;
		if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			struct wlr_scene_tree *tree = static_cast<struct wlr_scene_tree *>(xdg_surface->data);
			return static_cast<wb_toplevel *>(tree ? tree->node.data : NULL);
		}
		if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP &&
				xdg_surface->popup != NULL) {
			parent = xdg_surface->popup->parent;
		} else {
			return NULL;
		}
	}
	return NULL;
}

static void xdg_popup_commit(struct wb_popup *popup, void *data) {
	if (!popup->xdg_popup) return;
	struct wlr_xdg_surface *base = popup->xdg_popup->base;

	if (!base || !base->initial_commit)
		return;

	update_fractional_scale(base->surface);

	/* Unconstrain the popup against its toplevel's output. This must happen
	 * here, on the initial commit, rather than when the popup is created:
	 * wlr_xdg_popup_unconstrain_from_box() schedules a configure, which
	 * asserts the surface is initialized, and it is not yet initialized at
	 * creation time. */
	struct wb_toplevel *toplevel = popup_root_toplevel(popup->xdg_popup);
	if (toplevel != NULL) {
		struct wlr_output *wlr_output = wlr_output_layout_output_at(
				toplevel->server->output_layout,
				toplevel->geometry.x + popup->xdg_popup->current.geometry.x,
				toplevel->geometry.y + popup->xdg_popup->current.geometry.y);
		if (wlr_output != NULL && wlr_output->data != NULL) {
			struct wb_output *output = static_cast<struct wb_output *>(wlr_output->data);
			int top_margin = toplevel->server->config ?
				toplevel->server->config->margins.top : 0;
			struct wlr_box output_toplevel_box = {
				.x = output->geometry.x - toplevel->geometry.x,
				.y = output->geometry.y - toplevel->geometry.y,
				.width = output->geometry.width,
				.height = output->geometry.height - top_margin,
			};
			wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup,
					&output_toplevel_box);
			/* unconstrain_from_box schedules the configure for us. */
			return;
		}
	}

	wlr_xdg_surface_schedule_configure(base);
}

static void xdg_popup_destroy(struct wb_popup *popup, void *data) {
	if (popup->xdg_popup)
		update_fractional_scale(popup->xdg_popup->base->surface);
	/* The wb::Listener members disconnect themselves on delete. */
	delete popup;
}

static void handle_new_xdg_popup(struct wb_server *server, void *data) {
	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
	struct wlr_xdg_popup *xdg_popup = static_cast<struct wlr_xdg_popup *>(data);
	if (xdg_popup->parent) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(
			xdg_popup->parent);
		if (parent != NULL) {
			struct wlr_scene_tree *parent_tree = static_cast<struct wlr_scene_tree *>(parent->data);
			xdg_popup->base->data = wlr_scene_xdg_surface_create(
				parent_tree, xdg_popup->base);
		}
	}

	auto *popup = new wb_popup{};
	popup->xdg_popup = xdg_popup;
	popup->commit.connect(&xdg_popup->base->surface->events.commit,
			[popup](void *data) { xdg_popup_commit(popup, data); });
	popup->destroy.connect(&xdg_popup->events.destroy,
			[popup](void *data) { xdg_popup_destroy(popup, data); });
}

static void handle_new_xdg_toplevel(struct wb_server *server, void *data) {
	struct wlr_xdg_toplevel *xdg_toplevel = static_cast<struct wlr_xdg_toplevel *>(data);

	/* Allocate a wb_toplevel for this toplevel */
	auto *toplevel = new wb_toplevel{};
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;

	toplevel->foreign_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_list, &toplevel->foreign_toplevel_state);

	/* Connect each event to its handler. The handlers take the wb_toplevel
	 * directly; the lambda supplies it from the capture. */
	auto bind = [toplevel](wb::Listener &listener, struct wl_signal *signal,
			void (*handler)(struct wb_toplevel *, void *)) {
		listener.connect(signal, [toplevel, handler](void *data) {
			handler(toplevel, data);
		});
	};

	bind(toplevel->map, &xdg_toplevel->base->surface->events.map, xdg_toplevel_map);
	bind(toplevel->unmap, &xdg_toplevel->base->surface->events.unmap, xdg_toplevel_unmap);
	bind(toplevel->commit, &xdg_toplevel->base->surface->events.commit, xdg_toplevel_commit);
	bind(toplevel->destroy, &xdg_toplevel->events.destroy, xdg_toplevel_destroy);

	toplevel->scene_tree = wlr_scene_xdg_surface_create(
		&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	bind(toplevel->request_fullscreen, &xdg_toplevel->events.request_fullscreen, xdg_toplevel_request_fullscreen);
	bind(toplevel->request_maximize, &xdg_toplevel->events.request_maximize, xdg_toplevel_request_maximize);
	bind(toplevel->request_minimize, &xdg_toplevel->events.request_minimize, xdg_toplevel_request_minimize);
	bind(toplevel->request_move, &xdg_toplevel->events.request_move, xdg_toplevel_request_move);
	bind(toplevel->request_resize, &xdg_toplevel->events.request_resize, xdg_toplevel_request_resize);
	bind(toplevel->set_app_id, &xdg_toplevel->events.set_app_id, xdg_toplevel_set_app_id);
	bind(toplevel->set_title, &xdg_toplevel->events.set_title, xdg_toplevel_set_title);

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	wl_list_insert(&toplevel->server->focus_order, &toplevel->focus_link);
}

void init_xdg_shell(struct wb_server *server) {
	/* xdg-shell version 3 */
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
	server->new_xdg_popup.connect(&server->xdg_shell->events.new_popup,
			[server](void *data) { handle_new_xdg_popup(server, data); });
	server->new_xdg_toplevel.connect(&server->xdg_shell->events.new_toplevel,
			[server](void *data) { handle_new_xdg_toplevel(server, data); });
}
