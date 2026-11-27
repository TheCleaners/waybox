/*
 * Xwayland: X11 programs as Waybox windows.
 *
 * Xwayland starts lazily, on the first X11 connection; DISPLAY is exported
 * right away so programs Waybox starts can find it. Managed X11 windows are
 * wb_toplevels (kind Xwayland) and share mapping, placement, focus,
 * desktops, frames and actions with xdg-shell windows through the view
 * layer. Override-redirect windows (menus, tooltips, drag icons) are not
 * managed: they are drawn where the client puts them, above ordinary
 * windows, and get keyboard focus only when they ask for it.
 */
#include "waybox/wlroots.hpp"

#if WLR_HAS_XWAYLAND

#include <cstdlib>

#include "waybox/xwayland.h"
#include "waybox/frame_view.hpp"
#include "waybox/listener.hpp"
#include "waybox/seat.h"
#include "waybox/xdg_shell.h"
#include "waybox/output.h"
#include "config.h"
#include "decoration.h"

namespace {

/* The surface's first buffer can come (over Wayland) before Xwayland links
 * it to its window (over X11): the two aren't synchronised. wlroots 0.20.2
 * and older then only map it at its next commit, and a window that
 * doesn't redraw (a plain dialog) never shows. Map it now, as later
 * wlroots does (a no-op there: it's mapped already). True if this did. */
bool map_if_drawn(struct wlr_surface *surface) {
	if (surface->mapped || !wlr_surface_has_buffer(surface))
		return false;
	wlr_surface_map(surface);
	return true;
}

/* ---- Override-redirect (unmanaged) windows ---- */

struct Unmanaged {
	struct wb_server *server = nullptr;
	struct wlr_xwayland_surface *xsurface = nullptr;
	struct wlr_scene_tree *tree = nullptr;
	wb::Listener tree_destroy;
	wb::Listener associate;
	wb::Listener dissociate;
	wb::Listener map;
	wb::Listener unmap;
	wb::Listener set_geometry;
	wb::Listener set_override_redirect;
	wb::Listener destroy;
};

void new_managed(struct wb_server *server, struct wlr_xwayland_surface *xs);
void new_unmanaged(struct wb_server *server, struct wlr_xwayland_surface *xs);

void unmanaged_map(Unmanaged *u) {
	struct wlr_xwayland_surface *xs = u->xsurface;
	u->tree = wlr_scene_subsurface_tree_create(u->server->unmanaged_tree,
			xs->surface);
	if (u->tree != nullptr) {
		wlr_scene_node_set_position(&u->tree->node, xs->x, xs->y);
		/* wlroots frees the tree with the surface; forget it then. */
		u->tree_destroy.connect(&u->tree->node.events.destroy, [u](void *) {
			u->tree = nullptr;
			u->tree_destroy.disconnect();
		});
	}
	if (wlr_xwayland_surface_override_redirect_wants_focus(xs))
		seat_focus_surface(u->server->seat.get(), xs->surface);
}

void unmanaged_unmap(Unmanaged *u) {
	if (u->tree != nullptr) {
		u->tree_destroy.disconnect();
		wlr_scene_node_destroy(&u->tree->node);
		u->tree = nullptr;
	}
	struct wb_server *server = u->server;
	struct wlr_seat *seat = server->seat->seat;
	if (u->xsurface->surface != nullptr &&
			seat->keyboard_state.focused_surface == u->xsurface->surface) {
		/* A menu with focus went away: back to the window it came from. */
		if (struct wb_toplevel *active = first_toplevel(server))
			seat_focus_surface(server->seat.get(), view_surface(active));
		else
			seat_focus_surface(server->seat.get(), nullptr);
	}
}

/* Follow the window's wl_surface (also when it was associated, or even
 * mapped, before we got the window: see the override-redirect switch). */
void unmanaged_associate(Unmanaged *u) {
	struct wlr_surface *surface = u->xsurface->surface;
	u->map.connect(&surface->events.map, [u](void *) { unmanaged_map(u); });
	u->unmap.connect(&surface->events.unmap, [u](void *) { unmanaged_unmap(u); });
	if (!map_if_drawn(surface) && surface->mapped)  /* (mapping calls it) */
		unmanaged_map(u);
}

void new_unmanaged(struct wb_server *server, struct wlr_xwayland_surface *xs) {
	auto *u = new Unmanaged{};
	u->server = server;
	u->xsurface = xs;
	xs->data = nullptr;
	u->associate.connect(&xs->events.associate,
			[u](void *) { unmanaged_associate(u); });
	if (xs->surface != nullptr)
		unmanaged_associate(u);
	/* A window may stop being override-redirect before it is mapped (Wine
	 * does this, e.g. for game windows): it becomes an ordinary window. */
	u->set_override_redirect.connect(&xs->events.set_override_redirect,
			[u](void *) {
		if (u->xsurface->override_redirect)
			return;
		struct wb_server *server = u->server;
		struct wlr_xwayland_surface *xs = u->xsurface;
		unmanaged_unmap(u);
		delete u;  /* the Listener members disconnect themselves */
		new_managed(server, xs);
	});
	u->dissociate.connect(&xs->events.dissociate, [u](void *) {
		u->map.disconnect();
		u->unmap.disconnect();
	});
	u->set_geometry.connect(&xs->events.set_geometry, [u](void *) {
		if (u->tree != nullptr)
			wlr_scene_node_set_position(&u->tree->node, u->xsurface->x,
					u->xsurface->y);
	});
	u->destroy.connect(&xs->events.destroy, [u](void *) {
		if (u->tree != nullptr) {
			u->tree_destroy.disconnect();
			wlr_scene_node_destroy(&u->tree->node);
		}
		delete u;  /* the Listener members disconnect themselves */
	});
}

/* ---- Managed windows ---- */

/* Games often go "fullscreen" without saying so: an undecorated window
 * exactly the size of a monitor, with no _NET_WM_STATE_FULLSCREEN. Treat
 * that as fullscreen (as GNOME and KDE do), so it covers panels instead of
 * being fitted below them. Returns whether the box matches a monitor. */
bool covers_an_output(struct wb_server *server, struct wlr_xwayland_surface *xs,
		struct wlr_box want) {
	if (xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL)
		return false;  /* a decorated window is an ordinary big window */
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (!wlr_box_empty(&box) && wlr_box_equal(&box, &want))
			return true;
	}
	return false;
}

void x11_map(struct wb_toplevel *t) {
	struct wlr_xwayland_surface *xs = t->xsurface;
	/* Start from where and how big the client asked to be; map_toplevel()
	 * places it from there (and centres dialogs over their parent). */
	t->geometry = {xs->x, xs->y, xs->width, xs->height};
	bool screen_sized = covers_an_output(t->server, xs, t->geometry);
	t->surface_tree = wlr_scene_subsurface_tree_create(t->scene_tree, xs->surface);
	if (t->surface_tree != nullptr) {
		assign_scene_descriptor(&t->surface_tree->node,
				WB_SCENE_DESC_TOPLEVEL, t);
		/* wlroots frees the tree with the surface; forget it then. */
		t->surface_tree_destroy.connect(&t->surface_tree->node.events.destroy,
				[t](void *) {
			t->surface_tree = nullptr;
			t->surface_tree_destroy.disconnect();
		});
	}
	map_toplevel(t);
	if (screen_sized || xs->fullscreen) {
		/* Back over the monitor it asked for, then fullscreen there. */
		t->geometry = {xs->x, xs->y, xs->width, xs->height};
		position_toplevel(t);
		set_toplevel_fullscreen(t, true);
	}
}

void x11_unmap(struct wb_toplevel *t) {
	if (!t->mapped)
		return;
	unmap_toplevel(t);
	/* The capture scene shows the surface, which is going away. */
	drop_window_capture(t);
	/* Frames belong to a mapped window; the surface tree goes with the
	 * surface. */
	t->frame.reset();
	if (t->server->frame_pressed == t)
		t->server->frame_pressed = nullptr;
	if (t->surface_tree != nullptr) {
		t->surface_tree_destroy.disconnect();
		wlr_scene_node_destroy(&t->surface_tree->node);
		t->surface_tree = nullptr;
	}
}

void x11_commit(struct wb_toplevel *t) {
	struct wlr_xwayland_surface *xs = t->xsurface;
	/* The client may have resized itself (or acked our configure). */
	if (xs->width != t->geometry.width || xs->height != t->geometry.height) {
		t->geometry.width = xs->width;
		t->geometry.height = xs->height;
		if (t->frame)
			update_toplevel_decoration(t);
	}
}

void x11_request_configure(struct wb_toplevel *t, void *data) {
	auto *ev = static_cast<struct wlr_xwayland_surface_configure_event *>(data);
	if (!t->mapped) {
		/* Before mapping, the client chooses its own geometry; placement
		 * happens on map. */
		wlr_xwayland_surface_configure(t->xsurface, ev->x, ev->y,
				ev->width, ev->height);
		return;
	}
	struct wlr_box want = {ev->x, ev->y, ev->width, ev->height};
	if (!t->fullscreen && covers_an_output(t->server, t->xsurface, want)) {
		/* Resizing itself to cover a monitor: fullscreen there. */
		t->geometry = want;
		position_toplevel(t);
		set_toplevel_fullscreen(t, true);
		return;
	}
	if (t->fullscreen || t->max_horz || t->max_vert) {
		/* Managed size: repeat ours so the client stays in sync. */
		view_set_size(t, t->geometry.width, t->geometry.height);
		return;
	}
	/* A floating window may move and resize itself (many X11 programs
	 * restore their saved geometry this way). */
	t->geometry = {ev->x, ev->y, ev->width, ev->height};
	view_set_size(t, ev->width, ev->height);
	position_toplevel(t);
	update_toplevel_decoration(t);
}

/* Follow the window's wl_surface (also when it was associated, or even
 * mapped, before we got the window: see the override-redirect switch). */
void managed_associate(struct wb_toplevel *t) {
	struct wlr_surface *surface = t->xsurface->surface;
	t->map.connect(&surface->events.map, [t](void *) { x11_map(t); });
	t->unmap.connect(&surface->events.unmap, [t](void *) { x11_unmap(t); });
	t->commit.connect(&surface->events.commit, [t](void *) { x11_commit(t); });
	if (!map_if_drawn(surface) && surface->mapped)  /* (mapping calls it) */
		x11_map(t);
}

void new_managed(struct wb_server *server, struct wlr_xwayland_surface *xs) {
	auto *t = new wb_toplevel{};
	t->server = server;
	t->kind = wb_view_kind::Xwayland;
	t->xsurface = xs;
	xs->data = t;
	register_toplevel(t);

	t->associate.connect(&xs->events.associate,
			[t](void *) { managed_associate(t); });
	if (xs->surface != nullptr)
		managed_associate(t);
	/* The reverse switch: an ordinary window turning override-redirect. */
	t->set_override_redirect.connect(&xs->events.set_override_redirect,
			[t](void *) {
		if (!t->xsurface->override_redirect)
			return;
		struct wb_server *server = t->server;
		struct wlr_xwayland_surface *xs = t->xsurface;
		x11_unmap(t);
		xs->data = nullptr;
		t->xsurface = nullptr;
		destroy_toplevel(t);
		new_unmanaged(server, xs);
	});
	t->dissociate.connect(&xs->events.dissociate, [t](void *) {
		x11_unmap(t);  /* the surface is going away, mapped or not */
		t->map.disconnect();
		t->unmap.disconnect();
		t->commit.disconnect();
	});
	t->destroy.connect(&xs->events.destroy, [t](void *) {
		x11_unmap(t);
		t->xsurface->data = nullptr;
		/* Clear the pointer so the view layer treats this as gone while the
		 * shared teardown runs. */
		t->xsurface = nullptr;
		destroy_toplevel(t);
	});
	t->request_configure.connect(&xs->events.request_configure,
			[t](void *data) { x11_request_configure(t, data); });
	/* Only from a held button (a press on its own titlebar), and not when
	 * fullscreen: as for Wayland windows. */
	t->request_move.connect(&xs->events.request_move, [t](void *) {
		if (t->mapped && !t->fullscreen && t->server->cursor->buttons_down > 0)
			begin_interactive(t, WB_CURSOR_MOVE, 0);
	});
	t->request_resize.connect(&xs->events.request_resize, [t](void *data) {
		auto *ev = static_cast<struct wlr_xwayland_resize_event *>(data);
		if (t->mapped && !t->fullscreen && t->server->cursor->buttons_down > 0)
			begin_interactive(t, WB_CURSOR_RESIZE, ev->edges);
	});
	t->request_maximize.connect(&xs->events.request_maximize, [t](void *) {
		if (t->mapped)
			set_toplevel_maximized(t, t->xsurface->maximized_horz,
					t->xsurface->maximized_vert);
	});
	t->request_fullscreen.connect(&xs->events.request_fullscreen, [t](void *) {
		if (t->mapped)
			set_toplevel_fullscreen(t, t->xsurface->fullscreen);
	});
	t->request_minimize.connect(&xs->events.request_minimize, [t](void *data) {
		auto *ev = static_cast<struct wlr_xwayland_minimize_event *>(data);
		if (t->mapped)
			set_toplevel_minimized(t, ev->minimize);
	});
	/* _NET_ACTIVE_WINDOW: X11 can't show what input was behind it, so
	 * it's taken as the user's (as Sway does): focused on this desktop, on
	 * another marked urgent (smart, the default); <activation> decides. */
	t->request_activate.connect(&xs->events.request_activate, [t](void *) {
		toplevel_request_activation(t, true);
	});
	/* WM_HINTS' urgency flag: urgent while it's set. */
	t->set_hints.connect(&xs->events.set_hints, [t, xs](void *) {
		bool want = xs->hints != nullptr &&
				(xs->hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY) != 0;
		if (!t->mapped)
			return;
		struct wlr_surface *s = view_surface(t);
		bool focused = s != nullptr &&
				t->server->seat->seat->keyboard_state.focused_surface == s;
		set_toplevel_urgent(t, want && !focused);
	});
	t->set_title.connect(&xs->events.set_title,
			[t](void *) { toplevel_title_changed(t); });
	t->set_app_id.connect(&xs->events.set_class,
			[t](void *) { toplevel_app_id_changed(t); });
	t->set_decorations.connect(&xs->events.set_decorations,
			[t](void *) { update_toplevel_decoration(t); });
}

void handle_new_surface(struct wb_server *server, void *data) {
	auto *xs = static_cast<struct wlr_xwayland_surface *>(data);
	if (xs->override_redirect)
		new_unmanaged(server, xs);
	else
		new_managed(server, xs);
}

}  // namespace

void xwayland_update_cursor(struct wb_server *server) {
	if (server->xwayland == nullptr || server->cursor == nullptr ||
			server->cursor->xcursor_manager == nullptr)
		return;
	if (struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(
			server->cursor->xcursor_manager, "default", 1)) {
		struct wlr_xcursor_image *img = xc->images[0];
#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR < 20
		wlr_xwayland_set_cursor(server->xwayland, img->buffer,
				img->width * 4, img->width, img->height,
				static_cast<int32_t>(img->hotspot_x),
				static_cast<int32_t>(img->hotspot_y));
#else  /* 0.20 takes a wlr_buffer */
		wlr_xwayland_set_cursor(server->xwayland,
				wlr_xcursor_image_get_buffer(img),
				static_cast<int32_t>(img->hotspot_x),
				static_cast<int32_t>(img->hotspot_y));
#endif
	}
}

void init_xwayland(struct wb_server *server) {
	if (server->config != nullptr && !server->config->xwayland) {
		wlr_log(WLR_INFO, "%s", _("Xwayland disabled in rc.xml"));
		return;
	}
	server->xwayland = wlr_xwayland_create(server->wl_display,
			server->compositor, true /* lazy: start on first X11 client */);
	if (server->xwayland == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Could not set up Xwayland; X11 programs will not run"));
		return;
	}
	server->new_xwayland_surface.connect(&server->xwayland->events.new_surface,
			[server](void *data) { handle_new_surface(server, data); });
	server->xwayland_ready.connect(&server->xwayland->events.ready,
			[server](void *) {
		wlr_xwayland_set_seat(server->xwayland, server->seat->seat);
		/* The root window's cursor (X11 programs that never set one). */
		xwayland_update_cursor(server);
		wlr_log(WLR_INFO, "Xwayland ready on DISPLAY=%s",
				server->xwayland->display_name);
	});
	setenv("DISPLAY", server->xwayland->display_name, true);
	wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s (starts on first use)",
			server->xwayland->display_name);
}

void finish_xwayland(struct wb_server *server) {
	if (server->xwayland == nullptr)
		return;
	server->new_xwayland_surface.disconnect();
	server->xwayland_ready.disconnect();
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = nullptr;
}

#else  /* !WLR_HAS_XWAYLAND */

#include "waybox/xwayland.h"

void init_xwayland(struct wb_server *) {}
void finish_xwayland(struct wb_server *) {}
void xwayland_update_cursor(struct wb_server *) {}

#endif
