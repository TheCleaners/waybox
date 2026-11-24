#ifndef _WB_SERVER_H
#define _WB_SERVER_H

#include "waybox/wlroots.hpp"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TITLEBAR_HEIGHT 8 /* TODO: Get this from the theme */
#define WLR_CHECK_VERSION(major, minor, micro) (WLR_VERSION_NUM >= ((major << 16) | (minor << 8) | (micro)))

#include <stdlib.h>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef USE_NLS
#	include <libintl.h>
#	include <locale.h>
#	define _ gettext
#else
#	define _(s) (s)
#endif

#include "config.h"
#include "waybox/cursor.h"
#include "waybox/keychain.hpp"
#include "waybox/mousebind.hpp"
#include "decoration.h"
#include "layer_shell.h"
#include "waybox/xdg_shell.h"
#include "waybox/seat.h"

namespace wb { class MenuWidget; class SwitcherWidget; class DesktopPopup; }
struct wb_session_lock_state;
struct wb_workspace_state;

struct wb_input_method_relay;

struct wb_server {
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;

	struct wlr_allocator *allocator;
	struct wlr_backend *backend;
	struct wlr_compositor *compositor;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager;
	struct wlr_output_power_manager_v1 *output_power_manager;
	wb::Listener output_power_set_mode;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_output_layout *output_layout;
	struct wlr_xdg_output_manager_v1 *output_manager;
	struct wlr_renderer *renderer;
	/* text-input-v3 / input-method-v2 (see input_method.h). */
	struct wb_input_method_relay *input_method_relay = nullptr;
	/* xdg-toplevel-icon: icons clients set for their windows. */
	struct wlr_xdg_toplevel_icon_manager_v1 *toplevel_icon_manager = nullptr;
	wb::Listener set_toplevel_icon;
	/* security-context-v1: marks sandboxed clients (see sandbox.h). */
	struct wlr_security_context_manager_v1 *security_context_manager = nullptr;
	/* color-management-v1, when the renderer supports it (else null). */
	struct wlr_color_manager_v1 *color_manager = nullptr;
	struct wlr_scene *scene;
	/* Container for all xdg toplevels, kept at a fixed z between the lower
	 * (background, bottom) and upper (top, overlay) layer-shell trees so panels
	 * like Waybar correctly render above ordinary windows. */
	struct wlr_scene_tree *toplevel_tree;
	/* X11 override-redirect windows (menus, tooltips), above toplevels. */
	struct wlr_scene_tree *unmanaged_tree = nullptr;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_session *session;
	struct wlr_subcompositor *subcompositor;
	struct wlr_output_manager_v1 *wlr_output_manager;

	struct wb_config *config;
	char *config_file;

	std::unique_ptr<wb_cursor> cursor;
	std::unique_ptr<wb_seat> seat;

	struct wb_toplevel *grabbed_toplevel;
	/* While a mouse binding's actions run: the window it was bound to.
	 * Actions act on it rather than on the focused window, which may be a
	 * modal dialog in front of it. */
	struct wb_toplevel *action_window = nullptr;
	struct wlr_box grab_geo_box;
	double grab_x, grab_y;
	/* Where the pointer was when a move started (dragging a tiled window
	 * restores it only after the pointer has really moved). */
	double move_start_x = 0, move_start_y = 0;
	/* Press/Click/DoubleClick/Drag/Release detection for mouse bindings. */
	wb::ClickTracker click_tracker;
	/* Edge tiling during a move: the zone under the pointer and its
	 * translucent preview. */
	struct wlr_scene_rect *tile_preview = nullptr;
	int pending_tile = 0;  /* wb::TileZone */
	struct wlr_output *pending_tile_output = nullptr;
	uint32_t resize_edges;
	struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	struct wl_list toplevels;   /* wb_toplevel::link — stacking (z-)order, head = top */
	struct wl_list focus_order; /* wb_toplevel::focus_link — MRU focus order, head = active */

	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_shell *xdg_shell;

	wb::Listener new_layer_surface;
	wb::Listener new_xdg_decoration;
	wb::Listener new_xdg_popup;
	wb::Listener new_xdg_toplevel;

	wb::Listener destroy_inhibit_manager;
	wb::Listener new_inhibitor;

	wb::Listener new_input;
	wb::Listener new_output;
	wb::Listener output_layout_change;
	wb::Listener output_configuration_applied;
	wb::Listener output_configuration_tested;
	struct wl_list outputs; /* wb_output::link */

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
	wb::Listener new_virtual_keyboard;
	wb::Listener new_virtual_pointer;

	struct wlr_xdg_activation_v1 *xdg_activation;
	wb::Listener request_activate;

	/* Current node in the key-binding tree while resolving a key chain
	 * (nullptr = at the root, awaiting the first chord). */
	const wb::KeyBinding *active_keychain = nullptr;

	/* The open root/context menu (a compositor-drawn input grab), or null. */
	std::unique_ptr<wb::MenuWidget> menu;
	/* The window menu's contents (built each time it opens; the widget
	 * refers to them) and the window it was opened for, which its entries
	 * act on. */
	std::unique_ptr<wb::MenuFile> client_menu;
	struct wb_toplevel *menu_target = nullptr;

	/* The toplevel whose titlebar button is currently held down (for the
	 * pressed visual + activate-on-release), or null. */
	struct wb_toplevel *frame_pressed = nullptr;

	/* Interactive Alt+Tab task switcher (a keyboard grab) while a trigger
	 * modifier is held, or null. `switcher_windows` is the stable cyclable
	 * list parallel to the OSD entries; `switcher_mods` is the modifier mask
	 * whose release commits the selection. `last_key_modifiers` carries the
	 * modifiers of the key event currently firing an action into run_action. */
	std::unique_ptr<wb::SwitcherWidget> switcher;
	/* The desktop name shown after switching (<desktops><popupTime>). */
	std::unique_ptr<wb::DesktopPopup> desktop_popup;
	std::vector<struct wb_toplevel *> switcher_windows;
	uint32_t switcher_mods = 0;
	uint32_t last_key_modifiers = 0;

	/* Virtual desktops (0-based): the one shown, and the one shown before it
	 * (GoToDesktop's "last"). */
	int current_desktop = 0;
	int last_desktop = 0;

	/* ext-session-lock-v1 state (see session_lock.h), or null. */
	struct wb_session_lock_state *session_lock = nullptr;

	/* Window capture requests (see init_window_capture). */
	wb::Listener new_window_capture;

	/* Pointer constraints and relative motion (see pointer_constraints.h). */
	struct wlr_pointer_constraints_v1 *pointer_constraints = nullptr;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager = nullptr;
	struct wlr_pointer_constraint_v1 *active_constraint = nullptr;
	wb::Listener new_pointer_constraint;
	wb::Listener constraint_keyboard_focus;
	wb::Listener constraint_pointer_focus;

	/* Xwayland (see xwayland.h), or null when off or unavailable. */
	struct wlr_xwayland *xwayland = nullptr;
	wb::Listener new_xwayland_surface;
	wb::Listener xwayland_ready;

	/* ext-workspace-v1 state (see workspace.h), or null. */
	struct wb_workspace_state *workspaces = nullptr;
};

bool wb_create_backend(struct wb_server *server);
bool wb_start_server(struct wb_server *server);
bool wb_terminate(struct wb_server *server);
/* Run the actions of a chosen menu entry, on the window the menu was
 * opened for when it was the window menu. */
void wb_run_menu_actions(struct wb_server *server, std::vector<wb::Action> actions);
/* Open the window menu for the focused window (at the pointer when it is
 * over the window, else at the window's corner). */
void wb_show_client_menu(struct wb_server *server);
void wb_spawn(const char *cmd);
/* Reload rc.xml/menu.xml/theme and restyle existing window frames. */
void wb_reconfigure(struct wb_server *server);

#endif /* server.h */
