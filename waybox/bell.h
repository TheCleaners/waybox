#ifndef WB_BELL_H
#define WB_BELL_H

#include "waybox/server.h"

/* xdg-system-bell: programs ring the bell (a terminal's \a, GTK's error
 * beep). What it does is the user's choice, nothing by default:
 * <waybox><bell flash="yes" command="..."/> flashes the ringing window (or
 * the screen under the pointer) and/or runs a command. */
void init_bell(struct wb_server *server);
void finish_bell(struct wb_server *server);

/* Flashes are only drawn: hit tests (what's under the pointer) leave them
 * out, by turning them off around the search. */
void bell_flashes_set_enabled(bool enabled);

#endif
