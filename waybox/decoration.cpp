#include "decoration.h"
#include "waybox/xdg_shell.h"

/* waybox has no server-side decoration renderer yet, so SSD is not advertised.
 * Flip this when the SSD frame renderer lands and decoration negotiation will
 * start honouring DecorMode::Full / client SSD requests. */
static constexpr bool kSsdAvailable = false;

void apply_toplevel_decoration(struct wb_toplevel *toplevel) {
	if (toplevel == nullptr || toplevel->decoration == nullptr)
		return;
	struct wlr_xdg_toplevel_decoration_v1 *deco = toplevel->decoration;
	if (!deco->toplevel->base->initialized)
		return;

	bool client_wants_ssd = deco->requested_mode ==
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	wb::NegotiatedDecoration negotiated = wb::negotiate_decoration(
			toplevel->decorations, client_wants_ssd, kSsdAvailable);

	wlr_xdg_toplevel_decoration_v1_set_mode(deco,
			negotiated == wb::NegotiatedDecoration::ServerSide
				? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
				: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void handle_new_xdg_toplevel_decoration(struct wb_server *server, void *data) {
	auto *toplevel_decoration =
		static_cast<struct wlr_xdg_toplevel_decoration_v1 *>(data);

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
		struct wb_toplevel *toplevel =
			toplevel_from_node(scene_tree ? &scene_tree->node : nullptr);

		if (toplevel != nullptr) {
			toplevel->decoration = toplevel_decoration;
			apply_toplevel_decoration(toplevel);
		} else if (xdg_toplevel->base->initialized) {
			/* Couldn't resolve the window; fall back to the safe default so the
			 * client still gets a decoration configure. */
			wlr_xdg_toplevel_decoration_v1_set_mode(toplevel_decoration,
					WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		}
	});

	decoration->destroy.connect(&toplevel_decoration->events.destroy,
			[decoration](void *data) {
		/* The Listener members disconnect themselves in their destructors. */
		delete decoration;
	});
}

void init_xdg_decoration(struct wb_server *server) {
	struct wlr_xdg_decoration_manager_v1 *decoration = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_xdg_decoration.connect(&decoration->events.new_toplevel_decoration,
			[server](void *data) { handle_new_xdg_toplevel_decoration(server, data); });
}
