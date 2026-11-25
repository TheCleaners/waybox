#ifndef WB_STALL_WATCH_H
#define WB_STALL_WATCH_H

#include "waybox/server.h"

/* Logging of slow work, and (WAYBOX_STALL_WATCH=1) the event loop
 * watchdog; see stall_watch.cpp. */
void init_stall_watch(struct wb_server *server);
void finish_stall_watch(struct wb_server *server);

#endif
