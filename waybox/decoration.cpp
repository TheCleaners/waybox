#include "decoration.h"
#include "waybox/xdg_shell.h"

#include "config.h"
#include "waybox/frame_view.hpp"
#include "waybox/render.hpp"
#include "waybox/style.hpp"
#include "waybox/theme.hpp"

/* The SSD frame renderer now exists, so server-side decorations are advertised;
 * negotiation honours DecorMode::Full and client SSD requests. */
static constexpr bool kSsdAvailable = true;

/* Whether this toplevel should currently wear a server-side frame: the
 * negotiated decoration is server-side and it is not fullscreen. */
static bool toplevel_wants_ssd(struct wb_toplevel *toplevel) {
	if (toplevel->xdg_toplevel->current.fullscreen)
		return false;
	bool client_wants_ssd = toplevel->decoration != nullptr &&
			toplevel->decoration->requested_mode ==
					WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	return wb::negotiate_decoration(toplevel->decorations, client_wants_ssd,
			kSsdAvailable) == wb::NegotiatedDecoration::ServerSide;
}

/* Derive frame metrics from the theme, sizing the titlebar to the label font. */
static wb::FrameMetrics frame_metrics(const wb::Theme &theme,
		const wb::FrameStyle &style) {
	wb::FrameMetrics m;
	m.border = theme.border_width > 0 ? theme.border_width : 1;
	int text_h = wb::measure_text("Ag", style.label.font).height;
	/* Compact: just enough vertical padding to comfortably fit the title. */
	m.titlebar = text_h + 2 * theme.padding_y;
	if (m.titlebar < 16)
		m.titlebar = 16;
	m.button = m.titlebar - 6;
	if (m.button < 10)
		m.button = 10;
	m.title_pad = theme.padding_x > 0 ? theme.padding_x : 4;
	m.button_spacing = 2;
	m.corner = 10;
	return m;
}

void update_toplevel_decoration(struct wb_toplevel *toplevel) {
	if (toplevel == nullptr || toplevel->xdg_toplevel == nullptr ||
			!toplevel->xdg_toplevel->base->initialized)
		return;
	struct wb_server *server = toplevel->server;
	bool want = toplevel_wants_ssd(toplevel);

	if (want && !toplevel->frame) {
		static const wb::Theme kFallbackTheme = wb::default_theme();
		const wb::Theme &theme =
				server->config ? server->config->theme : kFallbackTheme;
		wb::FrameStyle active = wb::frame_style_from_theme(theme, true);
		wb::FrameStyle inactive = wb::frame_style_from_theme(theme, false);
		wb::FrameMetrics m = frame_metrics(theme, active);
		std::vector<wb::FrameButton> buttons = {wb::FrameButton::Iconify,
				wb::FrameButton::Maximize, wb::FrameButton::Close};
		toplevel->frame = std::make_unique<wb::FrameView>(
				toplevel->scene_tree, active, inactive, m, std::move(buttons));
	} else if (!want && toplevel->frame) {
		toplevel->frame.reset();
		if (toplevel->surface_tree != nullptr)
			wlr_scene_node_set_position(&toplevel->surface_tree->node, 0, 0);
	}

	if (toplevel->frame) {
		wb::FrameInsets in = toplevel->frame->insets();
		if (toplevel->surface_tree != nullptr)
			wlr_scene_node_set_position(&toplevel->surface_tree->node,
					in.left, in.top);
		struct wlr_box g = toplevel->xdg_toplevel->base->geometry;
		int cw = g.width > 0 ? g.width : toplevel->geometry.width;
		int ch = g.height > 0 ? g.height : toplevel->geometry.height;
		toplevel->frame->set_client_size(cw, ch);
		toplevel->frame->set_active(first_toplevel(server) == toplevel);
		const char *title = toplevel->xdg_toplevel->title;
		toplevel->frame->set_title(title != nullptr ? title : "");
		toplevel->frame->set_maximized(toplevel->max_horz && toplevel->max_vert);
	}

	position_toplevel(toplevel);
}

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

	/* Build/tear down the actual frame to match the negotiated mode. */
	update_toplevel_decoration(toplevel);
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
