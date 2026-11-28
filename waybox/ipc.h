#ifndef WB_IPC_H
#define WB_IPC_H

#include <string>

#include "waybox/server.h"
#include "waybox/json.hpp"

struct wb_toplevel;

/*
 * The scripting socket, $XDG_RUNTIME_DIR/waybox-$WAYLAND_DISPLAY.sock
 * (exported as WAYBOX_SOCK), when <waybox><ipc enabled="yes"/>. One JSON
 * object per line each way:
 *
 *   {"request": "get_windows"}   -> {"ok": true, "windows": [{"id", "app_id",
 *                                    "title", "desktop", "focused", ...}]}
 *   {"request": "get_desktops"}  {"request": "get_outputs"}
 *   {"request": "get_version"}
 *   {"request": "action", "action": "GoToDesktop", "to": "2"}
 *       (any rc.xml action; "command", "to", "direction", "wrap",
 *       "follow" as in rc.xml; "window": an id, "app_id:X", "title:X" or
 *       "focused" (the default))
 *   {"request": "pick_color"}  -> the next click's color: {"ok": true,
 *       "color": "#rrggbb"} (Escape, a right click, 30 s: an error)
 *   {"request": "subscribe", "events": ["window", "desktop", "output",
 *       "config", "bell"]}  -> then {"event": "window", "change": "new"|"close"|
 *       "focus"|"title", "window": {...}} and so on as they happen.
 *
 * Only the user running Waybox can connect.
 */
/* Start or stop it as the settings say (startup and Reconfigure). */
void ipc_reconfigured(struct wb_server *server);
/* The JSON the requests answer with, for the compositor's agent tools. */
wb::json::Value ipc_window_json(struct wb_toplevel *t);
wb::json::Value ipc_desktop_json(struct wb_server *server, int index);
wb::json::Value ipc_monitors_json(struct wb_server *server);
void ipc_finish(struct wb_server *server);

void ipc_window_event(struct wb_toplevel *toplevel, const char *change);
void ipc_desktop_event(struct wb_server *server);
void ipc_output_event(struct wb_server *server, struct wlr_output *output, const char *change);
/* A bell rang (from `t`, or no window), and what it showed. */
void ipc_bell_event(struct wb_server *server, struct wb_toplevel *t, const std::string &shown,
		int skipped);

#endif
