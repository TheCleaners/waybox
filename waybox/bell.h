#ifndef WB_BELL_H
#define WB_BELL_H

#include <string>

#include "waybox/server.h"

struct ScriptBell;

/* xdg-system-bell: programs ring the bell (a terminal's \a, GTK's error
 * beep). What it does is the user's choice, nothing by default:
 * <waybox><bell flash= color= duration= count= width= command=/> (or a Lua
 * "bell" policy, ring by ring) flashes the ringing window, its border or
 * the screen, or shows a note, and/or runs a command. Flashes are paced:
 * never more than 3 a second (bell_style.hpp). */
void init_bell(struct wb_server *server);
void finish_bell(struct wb_server *server);

/* A ring from `t` (or no window). With `preview` (wbconf's Test: settings
 * over rc.xml's), no policy is asked and no event sent. What was shown:
 * "window", "border", "screen", "osd", "paced" (let go) or "none". */
std::string bell_ring(struct wb_server *server, struct wb_toplevel *t,
		const ScriptBell *preview);

/* Flashes are only drawn: hit tests (what's under the pointer) leave them
 * out, by turning them off around the search. */
void bell_flashes_set_enabled(bool enabled);

#endif
