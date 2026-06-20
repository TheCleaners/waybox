#ifndef _WB_DECORATION_H
#define _WB_DECORATION_H

#include "waybox/wlroots.hpp"

#include "waybox/server.h"

struct wb_decoration {
	struct wb_server *server;
	struct wlr_xdg_toplevel_decoration_v1 *toplevel_decoration;

	struct wl_listener request_mode;
	struct wl_listener destroy;
};

void init_xdg_decoration(struct wb_server *server);
#endif
