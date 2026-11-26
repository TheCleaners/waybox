/*
 * More or less taken verbatim from wio <https://git.sr.ht/~sircmpwn/wio>.
 * Additional material taken from sway <https://github.com/swaywm/sway>.
 *
 * Copyright 2019 Drew DeVault
 * Copyright 2022 Sway Developers
 */
#include <vector>

#include "waybox/wlroots.hpp"

#include "waybox/xdg_shell.h"
#include "waybox/session_lock.h"

void assign_scene_descriptor(struct wlr_scene_node *node,
		enum wb_scene_descriptor_type type, void *data) {
	auto *desc = new wb_scene_descriptor{};
	desc->type = type;
	desc->data = data;

	desc->destroy.connect(&node->events.destroy, [desc](void *data) {
		/* The wb::Listener disconnects itself when the descriptor is deleted. */
		delete desc;
	});

	node->data = desc;
}

static void arrange_surface(struct wb_output *output, struct wlr_box *full_area,
		struct wlr_box *usable_area, struct wlr_scene_tree *scene_tree) {
	struct wlr_scene_node *node;
	wl_list_for_each(node, &scene_tree->children, link) {
		struct wb_scene_descriptor *desc = static_cast<struct wb_scene_descriptor *>(node->data);
		if (desc == NULL) {
			continue;
		}

		if (desc->type == WB_SCENE_DESC_LAYER_SHELL) {
			struct wb_layer_surface *surface = static_cast<struct wb_layer_surface *>(desc->data);
			/* Configure only after the client's initial commit (the
			 * protocol's order); that commit arranges it. */
			if (!surface->scene->layer_surface->initialized)
				continue;
			wlr_scene_layer_surface_v1_configure(surface->scene,
					full_area, usable_area);
		}
	}
}

void arrange_layers(struct wb_output *output) {
	/* Layer surfaces are arranged in output-local coordinates (full_area and
	 * usable_area start at 0,0); the output's layer trees sit at the output's
	 * position in the layout, so a second monitor's panel and wallpaper land
	 * on that monitor rather than at the origin. (The fullscreen tree holds
	 * toplevels, which use layout coordinates, so it stays at 0,0.) */
	wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &output->geometry);
	struct wlr_scene_tree *layer_trees[] = {
		output->layers.shell_background, output->layers.shell_bottom,
		output->layers.shell_top, output->layers.shell_overlay,
	};
	for (struct wlr_scene_tree *tree : layer_trees)
		wlr_scene_node_set_position(&tree->node, output->geometry.x,
				output->geometry.y);

	struct wlr_box usable_area = {};
	struct wlr_box full_area;
	wlr_output_effective_resolution(output->wlr_output,
			&usable_area.width, &usable_area.height);

	memcpy(&full_area, &usable_area, sizeof(struct wlr_box));

	arrange_surface(output, &full_area, &usable_area, output->layers.shell_background);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_bottom);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_top);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_overlay);

	/* Remember the area left over after subtracting layer-shell exclusive
	 * zones (e.g. a panel like Waybar) so that toplevels can be placed and
	 * maximized within it instead of underneath the panel. */
	bool changed = memcmp(&output->usable_area, &usable_area,
			sizeof(struct wlr_box)) != 0;
	output->usable_area = usable_area;

	/* A panel appeared, moved or went away: re-fit existing windows, so one
	 * opened before the panel started doesn't keep its titlebar underneath
	 * it, and maximized windows track the new area. */
	if (changed)
		arrange_toplevels(output->server);
}

static struct wlr_scene_tree *wb_layer_get_scene(struct wb_output *output,
		enum zwlr_layer_shell_v1_layer type) {
	switch (type) {
		case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
			return output->layers.shell_background;
		case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
			return output->layers.shell_bottom;
		case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
			return output->layers.shell_top;
		case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
			return output->layers.shell_overlay;
	}

	/* Unreachable */
	return NULL;
}

static struct wb_layer_surface *wb_layer_surface_create(
		struct wlr_scene_layer_surface_v1 *scene) {
	auto *surface = new wb_layer_surface{};
	surface->scene = scene;
	return surface;
}

static void handle_surface_commit(struct wb_layer_surface *surface, void *data) {
	if (!surface->output) {
		return;
	}
	wlr_fractional_scale_v1_notify_scale(surface->scene->layer_surface->surface, surface->output->wlr_output->scale);

	struct wlr_layer_surface_v1 *layer_surface = surface->scene->layer_surface;
	uint32_t committed = layer_surface->current.committed;

	enum zwlr_layer_shell_v1_layer layer_type = layer_surface->current.layer;
	struct wlr_scene_tree *output_layer = wb_layer_get_scene(
		surface->output, layer_type);

	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wlr_scene_node_reparent(&surface->scene->tree->node, output_layer);
	}

	if (committed || layer_surface->surface->mapped != surface->mapped) {
		surface->mapped = layer_surface->surface->mapped;
		arrange_layers(surface->output);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_surface_send_frame_done(layer_surface->surface, &now);
	}

	/* The layer trees keep the stacking they got when the output appeared
	 * (background and bottom below windows; top, fullscreen and overlay
	 * above). Raising one here would lift panels over fullscreen windows. */

	if (layer_surface == surface->server->seat->focused_layer) {
		seat_focus_surface(surface->server->seat.get(), layer_surface->surface);
	}
}

static void handle_map(struct wb_layer_surface *surface, void *data) {

	struct wlr_layer_surface_v1 *layer_surface =
				surface->scene->layer_surface;

	/* focus on new surface */
	if (layer_surface->current.keyboard_interactive &&
			(layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
			layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {

		struct wb_seat *seat = surface->server->seat.get();
		/* but only if the currently focused layer has a lower precedence */
		if (!seat->focused_layer ||
				seat->focused_layer->current.layer <= layer_surface->current.layer) {
			seat_set_focus_layer(seat, layer_surface);
		}
		if (surface->output) {
			arrange_layers(surface->output);
		}
	}
}

static void handle_unmap(struct wb_layer_surface *surface, void *data) {
	struct wb_seat *seat = surface->server->seat.get();
	if (seat->focused_layer == surface->scene->layer_surface) {
		seat_set_focus_layer(seat, NULL);
	}

	/* Give the keyboard back to a window only if this surface had it (a
	 * notification closing mustn't take it from an open launcher). */
	struct wlr_surface *focused = surface->server->seat->seat->keyboard_state.focused_surface;
	if (focused == surface->scene->layer_surface->surface &&
			!wl_list_empty(&surface->server->focus_order)) {
		struct wb_toplevel *toplevel = first_toplevel(surface->server);
		if (toplevel && toplevel->scene_tree && toplevel->scene_tree->node.enabled) {
			focus_toplevel(toplevel);
		}
	}
}

static void wb_layer_surface_destroy(struct wb_layer_surface *surface) {
	if (surface == NULL) {
		return;
	}
	/* Do not touch surface->scene->layer_surface here: this runs from the
	 * layer surface's destroy handler, by which point wlroots has already
	 * torn down the wlr_layer_surface_v1 and its scene helper. The wb::Listener
	 * members disconnect themselves on delete. */
	delete surface;
}

static void handle_destroy(struct wb_layer_surface *surface, void *data) {
	if (surface->output) {
		arrange_layers(surface->output);
	}
	wb_layer_surface_destroy(surface);
}

void layer_shell_output_destroy(struct wb_output *output) {
	/* Close every layer surface on this output before its layer trees go
	 * away. Otherwise they keep a dangling wb_output and the next commit
	 * (e.g. Waybar redrawing) dereferences freed memory. Closing sends
	 * `closed`; clients such as Waybar drop that bar and recreate it when the
	 * output comes back. Collect first: destroying modifies the scene. */
	std::vector<struct wb_layer_surface *> surfaces;
	struct wlr_scene_tree *trees[] = {
		output->layers.shell_background, output->layers.shell_bottom,
		output->layers.shell_top, output->layers.shell_fullscreen,
		output->layers.shell_overlay,
	};
	for (struct wlr_scene_tree *tree : trees) {
		struct wlr_scene_node *node;
		wl_list_for_each(node, &tree->children, link) {
			auto *desc = static_cast<struct wb_scene_descriptor *>(node->data);
			if (desc != NULL && desc->type == WB_SCENE_DESC_LAYER_SHELL)
				surfaces.push_back(static_cast<struct wb_layer_surface *>(desc->data));
		}
	}
	for (struct wb_layer_surface *surface : surfaces) {
		surface->output = NULL;
		wlr_layer_surface_v1_destroy(surface->scene->layer_surface);
	}
}

static void popup_handle_destroy(struct wb_layer_popup *popup, void *data) {
	/* The wb::Listener members disconnect themselves on delete. */
	delete popup;
}

static struct wb_layer_surface *popup_get_layer(
		struct wb_layer_popup *popup) {
	struct wlr_scene_node *current = &popup->scene->node;
	while (current) {
		if (current->data) {
			struct wb_scene_descriptor *desc = static_cast<struct wb_scene_descriptor *>(current->data);
			if (desc->type == WB_SCENE_DESC_LAYER_SHELL) {
				return static_cast<wb_layer_surface *>(desc->data);
			}
		}

		if (current->parent == NULL) {
			break;
		}
		current = &current->parent->node;
	}

	return NULL;
}

static void popup_unconstrain(struct wb_layer_popup *popup) {
	struct wb_layer_surface *surface = popup_get_layer(popup);
	if (!surface) {
		return;
	}

	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;
	struct wb_output *output = surface->output;
	if (output == NULL) {
		return;
	}

	/* The output box in the coordinates of the layer surface the popup
	 * belongs to (what wlroots unconstrains against): the output's layout
	 * box minus the surface's layout position. */
	int lx, ly;
	wlr_scene_node_coords(&surface->scene->tree->node, &lx, &ly);
	struct wlr_box output_toplevel_sx_box = {
		.x = output->geometry.x - lx,
		.y = output->geometry.y - ly,
		.width = output->geometry.width,
		.height = output->geometry.height,
	};

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static void popup_handle_new_popup(struct wb_layer_popup *popup, void *data);

static void popup_handle_commit(struct wb_layer_popup *popup, void *data) {
	/* Unconstrain on the first commit: doing it at creation time asserts in
	 * wlroots because the popup surface is not yet initialized. */
	if (popup->wlr_popup->base->initial_commit) {
		popup_unconstrain(popup);
	}
}

static struct wb_layer_popup *create_popup(struct wlr_xdg_popup *wlr_popup,
			struct wlr_scene_tree *parent) {
	auto *popup = new wb_layer_popup{};
	popup->wlr_popup = wlr_popup;

	popup->scene = wlr_scene_xdg_surface_create(parent, wlr_popup->base);
	if (!popup->scene) {
		delete popup;
		return nullptr;
	}

	assign_scene_descriptor(&popup->scene->node, WB_SCENE_DESC_LAYER_SHELL_POPUP,
			popup);

	popup->commit.connect(&wlr_popup->base->surface->events.commit,
			[popup](void *data) { popup_handle_commit(popup, data); });
	/* Placed anew by the client: kept on screen again. Let go when the
	 * popup role goes (wlroots asserts nothing listens then; the surface's
	 * destroy, below, may come after). */
	popup->reposition.connect(&wlr_popup->events.reposition,
			[popup](void *) {
				if (popup->wlr_popup->base->initialized)  /* else at its first commit */
					popup_unconstrain(popup);
			});
	popup->popup_destroy.connect(&wlr_popup->events.destroy, [popup](void *) {
		popup->reposition.disconnect();
		popup->popup_destroy.disconnect();
	});
	popup->destroy.connect(&wlr_popup->base->events.destroy,
			[popup](void *data) { popup_handle_destroy(popup, data); });
	popup->new_popup.connect(&wlr_popup->base->events.new_popup,
			[popup](void *data) { popup_handle_new_popup(popup, data); });

	return popup;
}

static void popup_handle_new_popup(struct wb_layer_popup *popup, void *data) {
	struct wlr_xdg_popup *wlr_popup = static_cast<struct wlr_xdg_popup *>(data);
	create_popup(wlr_popup, popup->scene);
}

static void handle_new_popup(struct wb_layer_surface *surface, void *data) {
	struct wlr_xdg_popup *wlr_popup = static_cast<struct wlr_xdg_popup *>(data);
	create_popup(wlr_popup, surface->scene->tree);
}

void handle_layer_shell_surface(struct wb_server *server, void *data) {
	struct wlr_layer_surface_v1 *layer_surface = static_cast<struct wlr_layer_surface_v1 *>(data);

	if (layer_surface->output == NULL) {
		/* The client didn't request a specific output, so pick one for it.
		 * Most layer clients (panels, launchers like rofi) rely on this.
		 * Prefer the output under the cursor, then the active toplevel's
		 * output, then simply the first connected output. */
		struct wlr_output *output = wlr_output_layout_output_at(
				server->output_layout,
				server->cursor->cursor->x, server->cursor->cursor->y);
		if (output == NULL) {
			struct wb_toplevel *toplevel = first_toplevel(server);
			if (toplevel != NULL) {
				output = get_active_output(toplevel);
			}
		}
		if (output == NULL && !wl_list_empty(&server->outputs)) {
			struct wb_output *first_output =
				wl_container_of(server->outputs.next, first_output, link);
			output = first_output->wlr_output;
		}
		layer_surface->output = output;
	}

	if (layer_surface->output == NULL || layer_surface->output->data == NULL) {
		/* We have no connected output at all; reject the surface rather than
		 * dereferencing a NULL output. */
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	struct wb_output *output = static_cast<struct wb_output *>(layer_surface->output->data);

	enum zwlr_layer_shell_v1_layer layer_type = layer_surface->pending.layer;
	struct wlr_scene_tree *output_layer = wb_layer_get_scene(
			output, layer_type);
	struct wlr_scene_layer_surface_v1 *scene_surface =
		wlr_scene_layer_surface_v1_create(output_layer, layer_surface);
	if (!scene_surface) {
		return;
	}

	struct wb_layer_surface *surface =
		wb_layer_surface_create(scene_surface);

	assign_scene_descriptor(&scene_surface->tree->node,
		WB_SCENE_DESC_LAYER_SHELL, surface);
	if (!scene_surface->tree->node.data) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	surface->output = output;
	surface->server = output->server;

	surface->surface_commit.connect(&layer_surface->surface->events.commit,
			[surface](void *data) { handle_surface_commit(surface, data); });
	surface->map.connect(&layer_surface->surface->events.map,
			[surface](void *data) { handle_map(surface, data); });
	surface->unmap.connect(&layer_surface->surface->events.unmap,
			[surface](void *data) { handle_unmap(surface, data); });
	surface->destroy.connect(&layer_surface->events.destroy,
			[surface](void *data) { handle_destroy(surface, data); });
	surface->new_popup.connect(&layer_surface->events.new_popup,
			[surface](void *data) { handle_new_popup(surface, data); });

}

void init_layer_shell(struct wb_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	server->new_layer_surface.connect(&server->layer_shell->events.new_surface,
			[server](void *data) { handle_layer_shell_surface(server, data); });
}
