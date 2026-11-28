#include "waybox/sandbox.h"

#include <set>
#include <string>

#include "waybox/wlroots.hpp"

#include "waybox/security.hpp"
#include "waybox/server.h"

static bool global_filter(const struct wl_client *client,
		const struct wl_global *global, void *data) {
	auto *server = static_cast<struct wb_server *>(data);
#if WLR_HAS_XWAYLAND
	/* Xwayland's own protocol is for Xwayland only. */
	if (server->xwayland != nullptr && server->xwayland->shell_v1 != nullptr &&
			global == server->xwayland->shell_v1->global)
		return server->xwayland->server != nullptr &&
				client == server->xwayland->server->client;
	if (global == server->xwayland_keyboard_grab)
		return server->xwayland != nullptr && server->xwayland->server != nullptr &&
				client == server->xwayland->server->client;
#else
	if (global == server->xwayland_keyboard_grab)
		return false;
#endif
	const char *name = wl_global_get_interface(global)->name;
	wb::ProtocolClass kind = wb::classify_protocol(name);
	if (kind == wb::ProtocolClass::Unknown) {
		/* Said once a name: a global nobody classified yet. */
		static std::set<std::string> said;
		if (said.insert(name).second)
			wlr_log(WLR_ERROR, "wb-global unclassified %s: not offered to sandboxed "
					"programs until it is (waybox/security.cpp)", name);
	}
	if (server->security_context_manager == nullptr)
		return true;
	const struct wlr_security_context_v1_state *context =
			wlr_security_context_manager_v1_lookup_client(
					server->security_context_manager,
					const_cast<struct wl_client *>(client));
	if (context == nullptr)
		return true;  /* not sandboxed */
	/* Sandboxed: only what's known to be ordinary (fail closed). */
	return kind == wb::ProtocolClass::Ordinary;
}

void init_sandbox(struct wb_server *server) {
	server->security_context_manager =
			wlr_security_context_manager_v1_create(server->wl_display);
	wl_display_set_global_filter(server->wl_display, global_filter, server);
}
