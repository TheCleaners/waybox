#ifndef _WB_XDG_SHELL_H
#define _WB_XDG_SHELL_H

#include "waybox/wlroots.hpp"

#include <memory>

#include "waybox/decor_mode.hpp"
#include "waybox/frame.hpp"
#include "waybox/listener.hpp"
#include "waybox/server.h"

namespace wb { class FrameView; }

struct wb_popup {
	struct wlr_xdg_popup *xdg_popup = nullptr;
	wb::Listener commit;
	wb::Listener destroy;
};

struct wb_toplevel {
	struct wb_server *server = nullptr;
	struct wlr_xdg_toplevel *xdg_toplevel = nullptr;
	/* scene_tree is the per-window frame container (positioned, raised/lowered
	 * and z-ordered as the window). surface_tree holds the client's xdg surface
	 * inside it, offset by the decoration insets (zero for client-side
	 * decorations). Server-side decoration nodes are siblings of surface_tree
	 * under scene_tree. */
	struct wlr_scene_tree *scene_tree = nullptr;
	struct wlr_scene_tree *surface_tree = nullptr;
	/* Server-side decoration view (titlebar/borders), or null for CSD. */
	std::unique_ptr<wb::FrameView> frame;

	struct wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;
	/* Per-window decoration preference (app rule / ToggleDecorations). */
	wb::DecorMode decorations = wb::DecorMode::Default;

	struct wlr_ext_foreign_toplevel_handle_v1 *foreign_toplevel_handle = nullptr;
	struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {};

	/* zwlr-foreign-toplevel-management handle (taskbar control), created while
	 * mapped. */
	struct wlr_foreign_toplevel_handle_v1 *wlr_foreign_handle = nullptr;
	wb::Listener foreign_request_maximize;
	wb::Listener foreign_request_minimize;
	wb::Listener foreign_request_fullscreen;
	wb::Listener foreign_request_activate;
	wb::Listener foreign_request_close;

	wb::Listener map;
	wb::Listener unmap;
	wb::Listener commit;
	wb::Listener destroy;
	wb::Listener new_popup;
	wb::Listener request_fullscreen;
	wb::Listener request_maximize;
	wb::Listener request_minimize;
	wb::Listener request_move;
	wb::Listener request_resize;
	wb::Listener set_app_id;
	wb::Listener set_title;

	struct wlr_box geometry = {};
	/* Per-state restore rects, kept separate so interleaving states
	 * (maximize/fullscreen/shade/minimize) each restore to the right place. */
	struct wlr_box restore_maximize = {};
	struct wlr_box restore_fullscreen = {};
	struct wlr_box restore_shade = {};
	struct wlr_box restore_minimize = {};
	/* Independent horizontal/vertical maximize state (the xdg protocol only
	 * signals full maximize, so we track the axes ourselves). */
	bool max_horz = false;
	bool max_vert = false;
	bool mapped = false;

	struct wl_list link;       /* server::toplevels — stacking (z-)order */
	struct wl_list focus_link; /* server::focus_order — MRU focus order */
};

void init_xdg_shell(struct wb_server *server);
void focus_toplevel(struct wb_toplevel *toplevel);
void raise_toplevel(struct wb_toplevel *toplevel);
void lower_toplevel(struct wb_toplevel *toplevel);
void set_toplevel_maximized(struct wb_toplevel *toplevel, bool horz, bool vert);
void set_toplevel_fullscreen(struct wb_toplevel *toplevel, bool fullscreen);
void arrange_toplevels(struct wb_server *server);
void constrain_toplevel_to_usable_area(struct wb_toplevel *toplevel);
void begin_interactive(struct wb_toplevel *toplevel,
		enum wb_cursor_mode mode, uint32_t edges);

/* Decoration insets for a toplevel (zero unless it has a server-side frame). */
wb::FrameInsets toplevel_insets(struct wb_toplevel *toplevel);
/* Position the frame container from toplevel->geometry, accounting for the
 * decoration insets so the *client* lands at geometry.x/y. Use this instead of
 * positioning scene_tree directly. */
void position_toplevel(struct wb_toplevel *toplevel);
/* Create/destroy and synchronise the server-side decoration frame from the
 * toplevel's negotiated decoration mode + current state (size/title/focus). */
void update_toplevel_decoration(struct wb_toplevel *toplevel);
/* If the topmost scene node at (lx, ly) is a server-side decoration, return its
 * toplevel and the frame part hit (in *hit); otherwise NULL (the point is a
 * client surface, or empty desktop). Respects z-order/occlusion. */
struct wb_toplevel *toplevel_frame_at(struct wb_server *server, double lx,
		double ly, wb::FrameHit *hit);
struct wb_toplevel *first_toplevel(struct wb_server *server);
/* The wb_toplevel a scene node carries via its wb_scene_descriptor, or NULL if
 * the node is not (the root of) one of our toplevels. */
struct wb_toplevel *toplevel_from_node(struct wlr_scene_node *node);
struct wlr_output *get_active_output(struct wb_toplevel *toplevel);
struct wb_toplevel *get_toplevel_at(
		struct wb_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
#endif
