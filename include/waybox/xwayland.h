#ifndef _WB_XWAYLAND_H
#define _WB_XWAYLAND_H

/* Xwayland support (see xwayland.cpp). No-ops when wlroots was built
 * without it. */

struct wb_server;

/* Create the lazily-started Xwayland server and export DISPLAY. Call after
 * the seat, cursor and config exist. */
void init_xwayland(struct wb_server *server);
/* Destroy it; call after clients are gone but before the seat/cursor. */
void finish_xwayland(struct wb_server *server);

#endif /* _WB_XWAYLAND_H */
