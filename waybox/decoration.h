#ifndef _WB_DECORATION_H
#define _WB_DECORATION_H

#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"
#include "waybox/server.h"

struct wb_decoration {
	struct wb_server *server;
	struct wlr_xdg_toplevel_decoration_v1 *toplevel_decoration;

	wb::Listener request_mode;
	wb::Listener destroy;
};

void init_xdg_decoration(struct wb_server *server);

/* Negotiate and apply the decoration mode for a toplevel from its current
 * wb::DecorMode preference and the client's requested mode. Safe to call any
 * time the preference changes (map, ToggleDecorations) or on request_mode. */
void apply_toplevel_decoration(struct wb_toplevel *toplevel);
#endif
