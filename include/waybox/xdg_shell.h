#ifndef _WB_XDG_SHELL_H
#define _WB_XDG_SHELL_H

#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"
#include "waybox/server.h"

struct wb_popup {
	struct wlr_xdg_popup *xdg_popup = nullptr;
	wb::Listener commit;
	wb::Listener destroy;
};

struct wb_toplevel {
	struct wb_server *server = nullptr;
	struct wlr_xdg_toplevel *xdg_toplevel = nullptr;
	struct wlr_scene_tree *scene_tree = nullptr;

	struct wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;

	struct wlr_ext_foreign_toplevel_handle_v1 *foreign_toplevel_handle = nullptr;
	struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {};

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
	struct wlr_box previous_geometry = {};

	struct wl_list link;
};

void init_xdg_shell(struct wb_server *server);
void focus_toplevel(struct wb_toplevel *toplevel);
void arrange_toplevels(struct wb_server *server);
void constrain_toplevel_to_usable_area(struct wb_toplevel *toplevel);
void begin_interactive(struct wb_toplevel *toplevel,
		enum wb_cursor_mode mode, uint32_t edges);
struct wb_toplevel *first_toplevel(struct wb_server *server);
struct wlr_output *get_active_output(struct wb_toplevel *toplevel);
struct wb_toplevel *get_toplevel_at(
		struct wb_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
#endif
