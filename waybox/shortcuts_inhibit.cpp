/*
 * keyboard-shortcuts-inhibit-v1 (see shortcuts_inhibit.h).
 */
#include "waybox/wlroots.hpp"

#include <set>

#include "config.h"
#include "waybox/session_lock.h"
#include "shortcuts_inhibit.h"

#include "xwayland-keyboard-grab-unstable-v1-protocol.h"

namespace {
struct Inhibitor {
	wb::Listener destroy;
};
}  // namespace

/* An Xwayland keyboard grab: an X11 program grabbed the keyboard
 * (XGrabKeyboard: VMs, remote desktops, some games), and Xwayland asks for
 * every key while its window has the keyboard: the X11 side of
 * keyboard-shortcuts-inhibit, under the same rules. */
struct XGrab {
	struct wb_server *server = nullptr;
	struct wl_resource *resource = nullptr;
	struct wlr_surface *surface = nullptr;
	wb::Listener surface_destroy;
};

struct wb_shortcuts_inhibit_state {
	/* Inhibitors (and Xwayland grabs) whose shortcuts were taken back with
	 * Super+Escape, until their surface loses the keyboard. */
	std::set<const void *> escaped;
	std::set<XGrab *> grabs;
	bool grab_active = false;  /* for the log */
};

static bool allowed(struct wb_server *server) {
	return server->config == nullptr || server->config->allow_shortcuts_inhibit;
}

void shortcuts_inhibit_update(struct wb_server *server) {
	if (server->shortcuts_inhibit == nullptr || server->shortcuts_inhibit_state == nullptr)
		return;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	bool grab = false;
	for (XGrab *g : server->shortcuts_inhibit_state->grabs) {
		if (g->surface != focused)
			server->shortcuts_inhibit_state->escaped.erase(g);
		else if (allowed(server) && !session_locked(server) &&
				server->shortcuts_inhibit_state->escaped.count(g) == 0)
			grab = true;
	}
	if (grab != server->shortcuts_inhibit_state->grab_active) {
		server->shortcuts_inhibit_state->grab_active = grab;
		wlr_log(WLR_INFO, "wb-xwayland-grab %s", grab ? "active" : "inactive");
	}
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	wl_list_for_each(in, &server->shortcuts_inhibit->inhibitors, link) {
		if (in->surface != focused)
			server->shortcuts_inhibit_state->escaped.erase(in);
		bool want = allowed(server) && in->surface == focused &&
				!session_locked(server) &&
				server->shortcuts_inhibit_state->escaped.count(in) == 0;
		if (want == in->active)
			continue;
		if (want)
			wlr_keyboard_shortcuts_inhibitor_v1_activate(in);
		else
			wlr_keyboard_shortcuts_inhibitor_v1_deactivate(in);
		wlr_log(WLR_INFO, "wb-shortcuts-inhibit %s", want ? "active" : "inactive");
	}
}

bool shortcuts_inhibited(struct wb_server *server) {
	if (server->shortcuts_inhibit == nullptr || server->shortcuts_inhibit_state == nullptr)
		return false;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	wl_list_for_each(in, &server->shortcuts_inhibit->inhibitors, link)
		if (in->surface == focused && in->active)
			return true;
	return server->shortcuts_inhibit_state->grab_active;
}

bool shortcuts_inhibit_toggle(struct wb_server *server) {
	if (server->shortcuts_inhibit == nullptr || server->shortcuts_inhibit_state == nullptr)
		return false;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	auto &escaped = server->shortcuts_inhibit_state->escaped;
	for (XGrab *g : server->shortcuts_inhibit_state->grabs) {
		if (g->surface != focused)
			continue;
		if (escaped.count(g))
			escaped.erase(g);
		else
			escaped.insert(g);
		shortcuts_inhibit_update(server);
		return true;
	}
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	wl_list_for_each(in, &server->shortcuts_inhibit->inhibitors, link) {
		if (in->surface != focused)
			continue;
		if (escaped.count(in))
			escaped.erase(in);
		else
			escaped.insert(in);
		shortcuts_inhibit_update(server);
		return true;
	}
	return false;
}

namespace {

void grab_forget(XGrab *g) {
	struct wb_server *server = g->server;
	server->shortcuts_inhibit_state->escaped.erase(g);
	server->shortcuts_inhibit_state->grabs.erase(g);
	g->surface_destroy.disconnect();
	g->surface = nullptr;
	shortcuts_inhibit_update(server);
}

void grab_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

const struct zwp_xwayland_keyboard_grab_v1_interface grab_impl = {
	.destroy = grab_destroy,
};

void grab_resource_destroy(struct wl_resource *resource) {
	auto *g = static_cast<XGrab *>(wl_resource_get_user_data(resource));
	if (g->surface != nullptr)
		grab_forget(g);
	delete g;
}

void manager_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void manager_grab_keyboard(struct wl_client *client, struct wl_resource *manager,
		uint32_t id, struct wl_resource *surface_resource, struct wl_resource *) {
	auto *server = static_cast<struct wb_server *>(wl_resource_get_user_data(manager));
	struct wl_resource *resource = wl_resource_create(client,
			&zwp_xwayland_keyboard_grab_v1_interface, wl_resource_get_version(manager), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	auto *g = new XGrab;
	g->server = server;
	g->resource = resource;
	g->surface = wlr_surface_from_resource(surface_resource);
	wl_resource_set_implementation(resource, &grab_impl, g, grab_resource_destroy);
	g->surface_destroy.connect(&g->surface->events.destroy, [g](void *) { grab_forget(g); });
	server->shortcuts_inhibit_state->grabs.insert(g);
	shortcuts_inhibit_update(server);
}

const struct zwp_xwayland_keyboard_grab_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.grab_keyboard = manager_grab_keyboard,
};

void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
			&zwp_xwayland_keyboard_grab_manager_v1_interface, static_cast<int>(version), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

}  // namespace

void init_shortcuts_inhibit(struct wb_server *server) {
	server->shortcuts_inhibit_state = new wb_shortcuts_inhibit_state;
	server->shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(server->wl_display);
	server->new_shortcuts_inhibitor.connect(&server->shortcuts_inhibit->events.new_inhibitor,
			[server](void *data) {
		auto *in = static_cast<struct wlr_keyboard_shortcuts_inhibitor_v1 *>(data);
		auto *wi = new Inhibitor;
		wi->destroy.connect(&in->events.destroy, [server, in, wi](void *) {
			server->shortcuts_inhibit_state->escaped.erase(in);
			delete wi;
		});
		shortcuts_inhibit_update(server);
	});
	server->keyboard_focus_change.connect(
			&server->seat->seat->keyboard_state.events.focus_change,
			[server](void *) { shortcuts_inhibit_update(server); });
	/* Only Xwayland sees it (sandbox.cpp's global filter). */
	server->xwayland_keyboard_grab = wl_global_create(server->wl_display,
			&zwp_xwayland_keyboard_grab_manager_v1_interface, 1, server, bind_manager);
}

void finish_shortcuts_inhibit(struct wb_server *server) {
	/* Grabs still held go with their clients; nothing may point here.
	 * (No state at all if Waybox failed to start before it was made.) */
	if (server->shortcuts_inhibit_state != nullptr) {
		for (XGrab *g : std::set<XGrab *>(server->shortcuts_inhibit_state->grabs)) {
			g->surface_destroy.disconnect();
			g->surface = nullptr;
		}
		server->shortcuts_inhibit_state->grabs.clear();
	}
	server->new_shortcuts_inhibitor.disconnect();
	server->keyboard_focus_change.disconnect();
	delete server->shortcuts_inhibit_state;
	server->shortcuts_inhibit_state = nullptr;
}
