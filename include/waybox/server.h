#ifndef _WB_SERVER_H
#define _WB_SERVER_H

#include "waybox/wlroots.hpp"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TITLEBAR_HEIGHT 8 /* TODO: Get this from the theme */
#define WLR_CHECK_VERSION(major, minor, micro) (WLR_VERSION_NUM >= ((major << 16) | (minor << 8) | (micro)))

#include <stdlib.h>

#ifdef USE_NLS
#	include <libintl.h>
#	include <locale.h>
#	define _ gettext
#else
#	define _(s) (s)
#endif

#include "config.h"
#include "waybox/cursor.h"
#include "decoration.h"
#include "layer_shell.h"
#include "waybox/xdg_shell.h"
#include "waybox/seat.h"

struct wb_server {
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;

	struct wlr_allocator *allocator;
	struct wlr_backend *backend;
	struct wlr_compositor *compositor;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_output_layout *output_layout;
	struct wlr_xdg_output_manager_v1 *output_manager;
	struct wlr_renderer *renderer;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_session *session;
	struct wlr_subcompositor *subcompositor;
	struct wlr_output_manager_v1 *wlr_output_manager;

	struct wb_config *config;
	char *config_file;

	std::unique_ptr<wb_cursor> cursor;
	std::unique_ptr<wb_seat> seat;

	struct wb_toplevel *grabbed_toplevel;
	struct wlr_box grab_geo_box;
	double grab_x, grab_y;
	uint32_t resize_edges;
	struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
	struct wl_list toplevels;   /* wb_toplevel::link — stacking (z-)order, head = top */
	struct wl_list focus_order; /* wb_toplevel::focus_link — MRU focus order, head = active */

	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_shell *xdg_shell;

	wb::Listener gamma_control_set_gamma;
	wb::Listener new_layer_surface;
	wb::Listener new_xdg_decoration;
	wb::Listener new_xdg_popup;
	wb::Listener new_xdg_toplevel;

	wb::Listener destroy_inhibit_manager;
	wb::Listener destroy_inhibitor;
	wb::Listener new_inhibitor;
	struct wl_list inhibitors;

	wb::Listener new_input;
	wb::Listener new_output;
	wb::Listener output_configuration_applied;
	wb::Listener output_configuration_tested;
	struct wl_list outputs; /* wb_output::link */

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
	wb::Listener new_virtual_keyboard;
	wb::Listener new_virtual_pointer;

	struct wlr_xdg_activation_v1 *xdg_activation;
	wb::Listener request_activate;
};

bool wb_create_backend(struct wb_server *server);
bool wb_start_server(struct wb_server *server);
bool wb_terminate(struct wb_server *server);
void wb_spawn(const char *cmd);

#endif /* server.h */
