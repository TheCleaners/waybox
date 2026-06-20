#include "waybox/wlroots.hpp"

#include "idle.h"
#include "waybox/xdg_shell.h"

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

/* Returns the front (most recently focused) toplevel, or NULL if there are no
 * toplevels. Callers must not assume server->toplevels is non-empty: indexing
 * server->toplevels.next when the list is empty yields the list sentinel cast
 * to a bogus wb_toplevel. */
struct wb_toplevel *first_toplevel(struct wb_server *server) {
	if (wl_list_empty(&server->toplevels)) {
		return NULL;
	}
	struct wb_toplevel *toplevel =
		wl_container_of(server->toplevels.next, toplevel, link);
	return toplevel;
}

void focus_toplevel(struct wb_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL || toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
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
		/* Don't focus a surface that's already focused. */
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
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	seat_focus_surface(server->seat, surface);
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

		struct wlr_box usable = get_usable_area(toplevel);
		if (usable.width <= 0 || usable.height <= 0)
			continue;
		struct wb_config *config = server->config;
		int ml = config ? config->margins.left : 0;
		int mr = config ? config->margins.right : 0;
		int mt = config ? config->margins.top : 0;
		int mb = config ? config->margins.bottom : 0;

		if (toplevel->xdg_toplevel->current.maximized) {
			toplevel->geometry.x = usable.x + ml;
			toplevel->geometry.y = usable.y + mt;
			toplevel->geometry.width = usable.width - ml - mr;
			toplevel->geometry.height = usable.height - mt - mb;
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
					toplevel->geometry.width, toplevel->geometry.height);
		} else {
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
		}
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

	/* Usable area in layout coordinates. */
	int ux = output_box.x + usable.x;
	int uy = output_box.y + usable.y;
	int uw = usable.width;
	int uh = usable.height;
	int w = toplevel->geometry.width;
	int h = toplevel->geometry.height;

	if (uy > output_box.y && toplevel->geometry.y < uy)
		toplevel->geometry.y = uy;                 /* top reserved */
	if (ux > output_box.x && toplevel->geometry.x < ux)
		toplevel->geometry.x = ux;                 /* left reserved */
	if (uy + uh < output_box.y + output_box.height &&
			toplevel->geometry.y + h > uy + uh)
		toplevel->geometry.y = uy + uh - h;        /* bottom reserved */
	if (ux + uw < output_box.x + output_box.width &&
			toplevel->geometry.x + w > ux + uw)
		toplevel->geometry.x = ux + uw - w;        /* right reserved */
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	if (toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;

	struct wb_config *config = toplevel->server->config;
	struct wlr_box usable_area = get_usable_area(toplevel);
	struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;

	if (config) {
		toplevel->geometry.height = MIN(geo_box.height,
				usable_area.height - config->margins.top - config->margins.bottom);
		toplevel->geometry.width = MIN(geo_box.width,
				usable_area.width - config->margins.left - config->margins.right);
		toplevel->geometry.x = usable_area.x + config->margins.left;
		toplevel->geometry.y = usable_area.y + config->margins.top;
	} else {
		toplevel->geometry.height = MIN(geo_box.height, usable_area.height);
		toplevel->geometry.width = MIN(geo_box.width, usable_area.width);
		toplevel->geometry.x = usable_area.x;
		toplevel->geometry.y = usable_area.y;
	}

	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->geometry.width, toplevel->geometry.height);
	focus_toplevel(toplevel);

	wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->geometry.x, toplevel->geometry.y);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	if (toplevel->xdg_toplevel->base->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;
	reset_cursor_mode(toplevel->server);

	/* Focus the next toplevel, if any. */
	if (toplevel->link.next != &toplevel->server->toplevels) {
		struct wb_toplevel *next_toplevel = wl_container_of(toplevel->link.next, next_toplevel, link);
		if (next_toplevel && next_toplevel->xdg_toplevel && next_toplevel->scene_tree && next_toplevel->scene_tree->node.enabled) {
			wlr_log(WLR_INFO, "%s: %s", _("Focusing next toplevel"),
					next_toplevel->xdg_toplevel->app_id);
			focus_toplevel(next_toplevel);
		}
	}
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

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
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

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed and should never be shown again. */
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	struct wlr_output *output = get_active_output(toplevel);
	struct wlr_xdg_surface *base = toplevel->xdg_toplevel->base;
	if (output != NULL)
		wlr_surface_send_leave(base->surface, output);
	update_fractional_scale(base->surface);
	wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->foreign_toplevel_handle);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);

	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->request_minimize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->set_app_id.link);
	wl_list_remove(&toplevel->set_title.link);

	wl_list_remove(&toplevel->link);
	free(toplevel);
}

static void xdg_toplevel_set_app_id(
		struct wl_listener *listener, void *data) {
	struct wb_toplevel *toplevel =
		wl_container_of(listener, toplevel, set_app_id);
	toplevel->foreign_toplevel_state.app_id = toplevel->xdg_toplevel->app_id;
	wlr_ext_foreign_toplevel_handle_v1_update_state(
			toplevel->foreign_toplevel_handle, &toplevel->foreign_toplevel_state);
}

static void xdg_toplevel_set_title(
		struct wl_listener *listener, void *data) {
	struct wb_toplevel *toplevel =
		wl_container_of(listener, toplevel, set_title);
	toplevel->foreign_toplevel_state.title = toplevel->xdg_toplevel->title;
	wlr_ext_foreign_toplevel_handle_v1_update_state(
			toplevel->foreign_toplevel_handle, &toplevel->foreign_toplevel_state);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to set itself to
	 * fullscreen. */
	struct wb_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	/* A client may request fullscreen as part of its initial state, before
	 * its first commit. Calling wlr_xdg_toplevel_set_* on an uninitialized
	 * surface aborts in wlroots, so ignore the request until it is mapped;
	 * the client can re-request once initialized. */
	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	bool is_fullscreen = toplevel->xdg_toplevel->current.fullscreen;
	if (!is_fullscreen) {
		struct wlr_output *wlr_output = get_active_output(toplevel);
		struct wb_output *output = static_cast<struct wb_output *>(wlr_output ? wlr_output->data : NULL);
		toplevel->previous_geometry = toplevel->geometry;
		toplevel->geometry.x = 0;
		toplevel->geometry.y = 0;
		if (output != NULL) {
			toplevel->geometry.height = output->geometry.height;
			toplevel->geometry.width = output->geometry.width;
		}
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	} else {
		toplevel->geometry = toplevel->previous_geometry;
	}

	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->geometry.width, toplevel->geometry.height);
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, !is_fullscreen);
	wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->geometry.x, toplevel->geometry.y);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations.
	 */
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
	/* Clients (e.g. Chromium) may request maximize before their first commit.
	 * wlr_xdg_toplevel_set_size()/set_maximized() assert the surface is
	 * initialized, so ignore the request until the surface is mapped. */
	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	struct wlr_box usable_area = get_usable_area(toplevel);

	bool is_maximized = toplevel->xdg_toplevel->current.maximized;
	if (!is_maximized) {
		struct wb_config *config = toplevel->server->config;
		toplevel->previous_geometry = toplevel->geometry;
		if (config) {
			toplevel->geometry.x = usable_area.x + config->margins.left;
			toplevel->geometry.y = usable_area.y + config->margins.top;
			usable_area.height -= config->margins.top + config->margins.bottom;
			usable_area.width -= config->margins.left + config->margins.right;
		} else {
			toplevel->geometry.x = usable_area.x;
			toplevel->geometry.y = usable_area.y;
		}
	} else {
		usable_area = toplevel->previous_geometry;
		toplevel->geometry.x = toplevel->previous_geometry.x;
		toplevel->geometry.y = toplevel->previous_geometry.y;
	}
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, usable_area.width, usable_area.height);
	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, !is_maximized);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->geometry.x, toplevel->geometry.y);
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
	bool minimize_requested = toplevel->xdg_toplevel->requested.minimized;
	if (minimize_requested) {
		toplevel->previous_geometry = toplevel->geometry;
		toplevel->geometry.y = -toplevel->geometry.height;

		if (toplevel->link.next != &toplevel->server->toplevels) {
			struct wb_toplevel *next_toplevel = wl_container_of(toplevel->link.next, next_toplevel, link);
			focus_toplevel(next_toplevel);
		} else {
			focus_toplevel(toplevel);
		}
	} else {
		toplevel->geometry = toplevel->previous_geometry;
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
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. */
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, WB_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. */
	struct wlr_xdg_toplevel_resize_event *event = static_cast<struct wlr_xdg_toplevel_resize_event *>(data);
	struct wb_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
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

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct wb_popup *popup = wl_container_of(listener, popup, commit);
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

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct wb_popup *popup = wl_container_of(listener, popup, destroy);
	if (popup->xdg_popup)
		update_fractional_scale(popup->xdg_popup->base->surface);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
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

	struct wb_popup *popup = static_cast<struct wb_popup *>(calloc(1, sizeof(struct wb_popup)));
	if (popup == NULL) {
		return;
	}
	popup->xdg_popup = xdg_popup;
	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wb_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = static_cast<struct wlr_xdg_toplevel *>(data);

	/* Allocate a wb_toplevel for this toplevel */
	struct wb_toplevel *toplevel =
		static_cast<wb_toplevel *>(calloc(1, sizeof(struct wb_toplevel)));
	if (toplevel == NULL) {
		return;
	}
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;

	toplevel->foreign_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_list, &toplevel->foreign_toplevel_state);

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->scene_tree = wlr_scene_xdg_surface_create(
		&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);
	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
}

void init_xdg_shell(struct wb_server *server) {
	/* xdg-shell version 3 */
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
	server->new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
	server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
}
