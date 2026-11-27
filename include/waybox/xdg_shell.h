#ifndef _WB_XDG_SHELL_H
#define _WB_XDG_SHELL_H

#include "waybox/wlroots.hpp"

#include <memory>
#include <optional>
#include <string>

#include "waybox/session_store.hpp"

#include <cairo.h>

#include "waybox/decor_mode.hpp"
#include "waybox/edge_tiling.hpp"
#include "waybox/frame.hpp"
#include "waybox/listener.hpp"
#include "waybox/server.h"

namespace wb { class FrameView; }

struct wb_popup {
	struct wlr_xdg_popup *xdg_popup = nullptr;
	wb::Listener commit;
	wb::Listener reposition;  /* xdg_popup.reposition: placed again, kept on screen */
	wb::Listener destroy;
};

/* What kind of client window a wb_toplevel wraps. Code outside the shell
 * files goes through the view_* functions below instead of touching
 * xdg_toplevel/xsurface directly. */
enum class wb_view_kind { Xdg, Xwayland };

struct wb_toplevel_session;

struct wb_toplevel {
	struct wb_server *server = nullptr;
	/* Session restore (xdg-session-management): its handle, and the state
	 * to restore it to when it maps (asked before its first commit). */
	struct wb_toplevel_session *session_handle = nullptr;
	std::optional<wb::SavedWindow> session_restore;
	bool session_restore_desktop = false;  /* recovering: its desktop too */
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
	/* The titlebar icon (owned; see window_icon.h), and what the client set
	 * with xdg-toplevel-icon: pixels (owned) or an icon name. */
	cairo_surface_t *icon = nullptr;
	cairo_surface_t *client_icon = nullptr;
	std::string client_icon_name;
	/* xdg-toplevel-tag: a stable, untranslated name the program gives this
	 * window ("preferences", "player"), and its description. */
	std::string tag, tag_description;

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
	wb::Listener set_hints;      /* WM_HINTS: urgency */
	wb::Listener set_override_redirect;
	wb::Listener surface_tree_destroy;

	struct wlr_box geometry = {};
	/* Per-state restore rects, kept separate so interleaving states
	 * (maximize/fullscreen/shade/minimize) each restore to the right place. */
	struct wlr_box restore_maximize = {};
	struct wlr_box restore_fullscreen = {};
	/* Independent horizontal/vertical maximize state (the xdg protocol only
	 * signals full maximize, so we track the axes ourselves). */
	bool max_horz = false;
	bool max_vert = false;
	bool mapped = false;
	bool minimized = false;
	bool fullscreen = false;
	bool shaded = false;
	/* Window capture (screen sharing one window): a private scene with
	 * just this window's contents, made on the first capture request. The
	 * capture source belongs to it and goes away with it. */
	struct wlr_scene *capture_scene = nullptr;
	struct wlr_ext_image_capture_source_v1 *capture_source = nullptr;
	/* Tiled to an edge zone (drag to an edge, SnapToEdge); restore_tile is
	 * the geometry from before, restored when it is dragged away. */
	bool tiled = false;
	wb::TileZone tile_zone = wb::TileZone::None;
	struct wlr_box restore_tile = {};
	/* Virtual desktop (0-based) the window lives on; an omnipresent window
	 * shows on every desktop (Openbox's "all desktops"/sticky). */
	int desktop = 0;
	bool omnipresent = false;
	/* Desktops per monitor: the monitor whose desktop it's on (by name);
	 * moved to another, it takes the desktop that one shows. */
	std::string desktop_output;
	/* The monitor it's on now (by name) and where on it; and, once that
	 * monitor went away (unplugged, switched off) and it was brought onto
	 * another, the one it goes back to when that returns (unless the user
	 * moves it meanwhile). */
	std::string on_output;
	int on_dx = 0, on_dy = 0;
	std::string home_output;
	int home_dx = 0, home_dy = 0;
	bool going_home = false;
	/* Openbox's layer: -1 always below, 0 normal, 1 always on top. Raising
	 * and lowering stay within it. */
	int layer = 0;
	/* Asked for attention (activation it didn't get, an X11 urgency hint):
	 * until it's focused. Its desktop shows as urgent (ext-workspace). */
	bool urgent = false;
	/* Minimized by ToggleShowDesktop: shown again when it's toggled off. */
	bool hidden_by_show_desktop = false;

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
/* A modal dialog: xdg-dialog's set_modal, or X11's _NET_WM_STATE_MODAL. */
bool view_is_modal(struct wb_toplevel *toplevel);
/* The mapped modal dialog in front of `toplevel` (the innermost, when a
 * modal dialog has its own), or null. */
struct wb_toplevel *view_modal_child(struct wb_toplevel *toplevel);
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
/* Roll a window up to its titlebar (hiding its contents) or back down.
 * Only windows with a server-side frame can be rolled up. */
void set_toplevel_shaded(struct wb_toplevel *toplevel, bool shaded);
/* The scene tree a window's layer lives in (-1, 0, 1), and moving it there:
 * on top of that layer (a fullscreen window stays in the fullscreen layer
 * while active). */
struct wlr_scene_tree *toplevel_layer_tree(struct wb_server *server, int layer);
void set_toplevel_layer(struct wb_toplevel *toplevel, int layer);
/* Insert a new toplevel into the server lists and create its scene
 * container (both kinds). */
void register_toplevel(struct wb_toplevel *toplevel);
/* Window capture (wlroots 0.20+; no-ops otherwise). */
void init_window_capture(struct wb_server *server);
void drop_window_capture(struct wb_toplevel *toplevel);
void focus_toplevel(struct wb_toplevel *toplevel);
void raise_toplevel(struct wb_toplevel *toplevel);
void lower_toplevel(struct wb_toplevel *toplevel);
/* `raise`: a newly maximized window comes to the front; re-fitting one
 * (arrange_toplevels) leaves the stacking alone. */
void set_toplevel_maximized(struct wb_toplevel *toplevel, bool horz, bool vert,
		bool raise = true);
void set_toplevel_fullscreen(struct wb_toplevel *toplevel, bool fullscreen);
void arrange_toplevels(struct wb_server *server);
void constrain_toplevel_to_usable_area(struct wb_toplevel *toplevel);
void begin_interactive(struct wb_toplevel *toplevel,
		enum wb_cursor_mode mode, uint32_t edges);
/* The corner nearest (x, y), as WLR_EDGE_* bits, for resizing from it. */
uint32_t nearest_resize_edges(struct wb_toplevel *toplevel, double x, double y);
/* Whether `toplevel` is still one of the server's windows. */
bool toplevel_alive(struct wb_server *server, struct wb_toplevel *toplevel);
/* Shown if it's on the current desktop and not minimized, else hidden. */
void toplevel_update_visibility(struct wb_toplevel *toplevel);

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
/* A window's frame (its outer rectangle, decorations included), and moving
 * it to another (out of maximized or tiled first; within its size limits). */
wb::Rect toplevel_frame(struct wb_toplevel *toplevel);
void set_toplevel_frame(struct wb_toplevel *toplevel, wb::Rect frame);
/* The frame rectangle `zone` covers on `output`, for the drag preview. */
wb::Rect tile_preview_rect(struct wb_server *server, struct wlr_output *output,
		wb::TileZone zone);

/* Virtual desktops. */
int desktop_count(struct wb_server *server);
bool toplevel_on_desktop(struct wb_toplevel *toplevel, int desktop);
bool toplevel_on_current_desktop(struct wb_toplevel *toplevel);
/* The desktop `output` shows (#367); the shared one for none. */
int output_desktop(struct wb_server *server, struct wlr_output *output);
/* Monitors added, removed, switched off or moved: with desktops per
 * monitor, each window's monitor looked at again (a gone one's windows
 * carry over by desktop number). */
void desktops_outputs_changed(struct wb_server *server);
/* A monitor back (plugged in, switched on): the windows moved off it when
 * it went go back to where they were on it. */
void windows_return_home(struct wb_server *server);
/* Whether each monitor shows its own desktop (<desktops><perMonitor>). */
bool desktops_per_monitor(struct wb_server *server);
/* The monitor desktop actions act on: the focused window's, else the
 * pointer's. */
struct wb_output *desktop_action_output(struct wb_server *server);
/* The current and last desktop desktop actions see (that monitor's;
 * shared: the server's). */
void desktop_action_state(struct wb_server *server, int *current, int *last);
/* A window asks to be shown (xdg-activation, X11 _NET_ACTIVE_WINDOW):
 * focused, or marked urgent, as <activation> says; `from_user`: made from
 * input (a click, a key), not by the program on its own. */
void toplevel_request_activation(struct wb_toplevel *toplevel, bool from_user);
void set_toplevel_urgent(struct wb_toplevel *toplevel, bool urgent);
/* ToggleShowDesktop: the current desktop's windows minimized, or (on again)
 * those brought back. Focusing a window ends it, the others left minimized
 * (as in Openbox). */
/* Every window shown minimized (or back); `only`: that monitor's alone. */
void toggle_show_desktop(struct wb_server *server, struct wb_output *only = nullptr);
/* AddDesktop, RemoveDesktop: one more after the current or the last; the
 * current or the last taken away, its windows to the one before. Until the
 * next Reconfigure (rc.xml keeps its number). */
void add_desktop(struct wb_server *server, bool after_current);
void remove_desktop(struct wb_server *server, bool current);
/* The process behind a Wayland window (its connection's credentials, which
 * a program can't fake), or -1 (an X11 window's _NET_WM_PID is only its own
 * say); `uid` its owner's. */
pid_t view_pid(struct wb_toplevel *toplevel, uid_t *uid);
/* Keyboard focus taken from the window, if it has it (Unfocus). */
void unfocus_toplevel(struct wb_toplevel *toplevel);
/* To the end of the focus order (FocusToBottom). */
void focus_order_to_bottom(struct wb_toplevel *toplevel);
/* Show desktop `index` (0-based): hide the other desktops' windows and focus
 * the most recently used window here (or nothing). */
void wb_go_to_desktop(struct wb_server *server, int index);
/* Desktop `index` on monitor `output` (desktops per monitor; shared: on all). */
void wb_go_to_desktop_on(struct wb_server *server, struct wb_output *output, int index);
/* Desktop `index` on every monitor (desktops per monitor: each switched). */
void wb_go_to_desktop_everywhere(struct wb_server *server, int index);
/* An output's usable area (less panels' reservations), in layout
 * coordinates; empty for nullptr. */
struct wlr_box output_usable_area(struct wb_server *server, struct wlr_output *output);
/* A <monitor> as window rules and ShowMenu's <position> say one: a number
 * (1, the leftmost), a name, "mouse", "active", "primary"; nullptr for
 * none such. */
struct wlr_output *rule_output(struct wb_server *server, const std::string &spec);
/* Show `text` for a moment, centred, in the on-screen display style (the
 * desktop popup's; showing again replaces it). */
void wb_show_popup(struct wb_server *server, const std::string &text, int duration_ms);
/* The on-screen displays' style: the theme's, with rc.xml's <osd> over it. */
wb::SwitcherStyle wb_osd_style(struct wb_server *server);
/* Each monitor's name on it, large (or only `only`'s, if not ""), for
 * `duration_ms`; how many were shown. */
int wb_identify_outputs(struct wb_server *server, const std::string &only, int duration_ms);
/* Move a window to desktop `index`; with `follow`, go there with it. */
void wb_send_to_desktop(struct wb_toplevel *toplevel, int index, bool follow);
void wb_toggle_omnipresent(struct wb_toplevel *toplevel);
/* Focus the most recently used visible window on the current desktop, or
 * clear keyboard focus when there is none. */
void focus_current_desktop(struct wb_server *server);
struct wb_output;
void focus_current_desktop_on(struct wb_server *server, struct wb_output *on);
/* After a reconfigure: move windows off desktops that no longer exist. */
void desktops_reconfigured(struct wb_server *server);
struct wb_toplevel *get_toplevel_at(
		struct wb_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
#endif
