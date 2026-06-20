#include "decoration.h"
#include "waybox/xdg_shell.h"

static void handle_new_xdg_toplevel_decoration(struct wl_listener *listener, void *data) {
	auto *toplevel_decoration =
		static_cast<struct wlr_xdg_toplevel_decoration_v1 *>(data);
	struct wb_server *server = wl_container_of(listener, server, new_xdg_decoration);

	auto *decoration = new wb_decoration{};
	decoration->server = server;
	decoration->toplevel_decoration = toplevel_decoration;

	decoration->request_mode.connect(&toplevel_decoration->events.request_mode,
			[decoration](void *data) {
		struct wlr_xdg_toplevel_decoration_v1 *toplevel_decoration =
			decoration->toplevel_decoration;
		struct wlr_xdg_toplevel *xdg_toplevel = toplevel_decoration->toplevel;

		/* Resolve the wb_toplevel that actually owns this decoration, rather
		 * than assuming it is the front toplevel. */
		auto *scene_tree =
			static_cast<struct wlr_scene_tree *>(xdg_toplevel->base->data);
		auto *toplevel = static_cast<struct wb_toplevel *>(
				scene_tree ? scene_tree->node.data : nullptr);

		if (xdg_toplevel->base->initialized)
			wlr_xdg_toplevel_decoration_v1_set_mode(toplevel_decoration,
					WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		if (toplevel != nullptr)
			toplevel->decoration = toplevel_decoration;
	});

	decoration->destroy.connect(&toplevel_decoration->events.destroy,
			[decoration](void *data) {
		/* The Listener members disconnect themselves in their destructors. */
		delete decoration;
	});
}

void init_xdg_decoration(struct wb_server *server) {
	struct wlr_xdg_decoration_manager_v1 *decoration = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_xdg_decoration.notify = handle_new_xdg_toplevel_decoration;
	wl_signal_add(&decoration->events.new_toplevel_decoration, &server->new_xdg_decoration);
}
