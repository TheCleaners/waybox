/* What the about menu (or card) shows: what is running. */
#ifndef WB_ABOUT_H
#define WB_ABOUT_H

#include "waybox/client_menu.hpp"

struct wb_server;

wb::AboutInfo about_info(struct wb_server *server);

#endif
