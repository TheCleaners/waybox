/*
 * keyboard-shortcuts-inhibit-v1 (see shortcuts_inhibit.h).
 */
#include "waybox/wlroots.hpp"

#include <set>

#include "config.h"
#include "waybox/session_lock.h"
#include "shortcuts_inhibit.h"

namespace {
struct Inhibitor {
	wb::Listener destroy;
};
}  // namespace

struct wb_shortcuts_inhibit_state {
	/* Inhibitors whose shortcuts were taken back with Super+Escape, until
	 * their surface loses the keyboard. */
	std::set<struct wlr_keyboard_shortcuts_inhibitor_v1 *> escaped;
};

static bool allowed(struct wb_server *server) {
	return server->config == nullptr || server->config->allow_shortcuts_inhibit;
}

void shortcuts_inhibit_update(struct wb_server *server) {
	if (server->shortcuts_inhibit == nullptr)
		return;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
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
	if (server->shortcuts_inhibit == nullptr)
		return false;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	wl_list_for_each(in, &server->shortcuts_inhibit->inhibitors, link)
		if (in->surface == focused && in->active)
			return true;
	return false;
}

bool shortcuts_inhibit_toggle(struct wb_server *server) {
	if (server->shortcuts_inhibit == nullptr)
		return false;
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	wl_list_for_each(in, &server->shortcuts_inhibit->inhibitors, link) {
		if (in->surface != focused)
			continue;
		auto &escaped = server->shortcuts_inhibit_state->escaped;
		if (escaped.count(in))
			escaped.erase(in);
		else
			escaped.insert(in);
		shortcuts_inhibit_update(server);
		return true;
	}
	return false;
}

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
}

void finish_shortcuts_inhibit(struct wb_server *server) {
	server->new_shortcuts_inhibitor.disconnect();
	server->keyboard_focus_change.disconnect();
	delete server->shortcuts_inhibit_state;
	server->shortcuts_inhibit_state = nullptr;
}
