#ifndef _WB_XDG_SHELL_H
#define _WB_XDG_SHELL_H

#include "waybox/wlroots.hpp"

#include <memory>

#include "waybox/decor_mode.hpp"
#include "waybox/edge_tiling.hpp"
#include "waybox/frame.hpp"
#include "waybox/listener.hpp"
#include "waybox/server.h"

namespace wb { class FrameView; }

struct wb_popup {
	struct wlr_xdg_popup *xdg_popup = nullptr;
	wb::Listener commit;
	wb::Listener destroy;
};

/* What kind of client window a wb_toplevel wraps. Code outside the shell
 * files goes through the view_* functions below instead of touching
 * xdg_toplevel/xsurface directly. */
enum class wb_view_kind { Xdg, Xwayland };

struct wb_toplevel {
	struct wb_server *server = nullptr;
	wb_view_kind kind = wb_view_kind::Xdg;
	struct wlr_xdg_toplevel *xdg_toplevel = nullptr;      /* Xdg only */
	struct wlr_xwayland_surface *xsurface = nullptr;      /* Xwayland only */
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
	/* Xwayland only. */
	wb::Listener associate;
	wb::Listener dissociate;
	wb::Listener request_configure;
	wb::Listener request_activate;
	wb::Listener set_decorations;
	wb::Listener set_override_redirect;
	wb::Listener surface_tree_destroy;

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
	bool minimized = false;
	bool fullscreen = false;
	/* Tiled to an edge zone (drag to an edge, SnapToEdge); restore_tile is
	 * the geometry from before, restored when it is dragged away. */
	bool tiled = false;
	wb::TileZone tile_zone = wb::TileZone::None;
	struct wlr_box restore_tile = {};
	/* Virtual desktop (0-based) the window lives on; an omnipresent window
	 * shows on every desktop (Openbox's "all desktops"/sticky). */
	int desktop = 0;
	bool omnipresent = false;

	struct wl_list link;       /* server::toplevels — stacking (z-)order */
	struct wl_list focus_link; /* server::focus_order — MRU focus order */
};

void init_xdg_shell(struct wb_server *server);

/* ---- Views: the per-kind window operations (view.cpp) ---- */
/* Title and app id (X11: WM_CLASS class); never null. */
const char *view_title(struct wb_toplevel *toplevel);
const char *view_app_id(struct wb_toplevel *toplevel);
/* The client's main surface, or null (an X11 window not yet associated). */
struct wlr_surface *view_surface(struct wb_toplevel *toplevel);
/* Whether configures may be sent (xdg: initial commit done). */
bool view_ready(struct wb_toplevel *toplevel);
/* The client's window geometry within its surface (xdg: excludes CSD
 * shadows); X11 windows have no offset. */
struct wlr_box view_geometry(struct wb_toplevel *toplevel);
/* Min/max size hints (0 = unset). */
void view_size_hints(struct wb_toplevel *toplevel, int *min_w, int *min_h,
		int *max_w, int *max_h);
/* Ask the client for a new size. X11 windows also learn their position
 * (toplevel->geometry.x/y), which they need for placing their own menus. */
void view_set_size(struct wb_toplevel *toplevel, int width, int height);
void view_set_activated(struct wb_toplevel *toplevel, bool activated);
/* Tell the client where it is after a move (X11 windows place their own
 * menus and popups from it; no-op for xdg-shell, whose clients don't know
 * their position). */
void view_moved(struct wb_toplevel *toplevel);
void view_set_maximized(struct wb_toplevel *toplevel, bool horz, bool vert);
void view_set_fullscreen(struct wb_toplevel *toplevel, bool fullscreen);
void view_set_minimized(struct wb_toplevel *toplevel, bool minimized);
void view_close(struct wb_toplevel *toplevel);
/* The window this one is a dialog/transient for, if it is one of ours. */
struct wb_toplevel *view_parent(struct wb_toplevel *toplevel);
/* The wb_toplevel whose main surface is `surface`, or null. */
struct wb_toplevel *toplevel_from_surface(struct wb_server *server,
		struct wlr_surface *surface);

/* ---- Shared window lifecycle (xdg_shell.cpp), used by both kinds ---- */
void map_toplevel(struct wb_toplevel *toplevel);
void unmap_toplevel(struct wb_toplevel *toplevel);
void destroy_toplevel(struct wb_toplevel *toplevel);
void toplevel_title_changed(struct wb_toplevel *toplevel);
void toplevel_app_id_changed(struct wb_toplevel *toplevel);
void set_toplevel_minimized(struct wb_toplevel *toplevel, bool minimized);
/* Insert a new toplevel into the server lists and create its scene
 * container (both kinds). */
void register_toplevel(struct wb_toplevel *toplevel);
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

/* Edge tiling. tile_toplevel() tiles to `zone` on `output` (null: the
 * window's own); TileZone::Top maximizes. untile_toplevel() restores the
 * size from before (and the position too with `restore_position`). */
void tile_toplevel(struct wb_toplevel *toplevel, wb::TileZone zone,
		struct wlr_output *output);
void untile_toplevel(struct wb_toplevel *toplevel, bool restore_position);
/* The frame rectangle `zone` covers on `output`, for the drag preview. */
wb::Rect tile_preview_rect(struct wb_server *server, struct wlr_output *output,
		wb::TileZone zone);

/* Virtual desktops. */
int desktop_count(struct wb_server *server);
bool toplevel_on_desktop(struct wb_toplevel *toplevel, int desktop);
bool toplevel_on_current_desktop(struct wb_toplevel *toplevel);
/* Show desktop `index` (0-based): hide the other desktops' windows and focus
 * the most recently used window here (or nothing). */
void wb_go_to_desktop(struct wb_server *server, int index);
/* Move a window to desktop `index`; with `follow`, go there with it. */
void wb_send_to_desktop(struct wb_toplevel *toplevel, int index, bool follow);
void wb_toggle_omnipresent(struct wb_toplevel *toplevel);
/* Focus the most recently used visible window on the current desktop, or
 * clear keyboard focus when there is none. */
void focus_current_desktop(struct wb_server *server);
/* After a reconfigure: move windows off desktops that no longer exist. */
void desktops_reconfigured(struct wb_server *server);
struct wb_toplevel *get_toplevel_at(
		struct wb_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
#endif
