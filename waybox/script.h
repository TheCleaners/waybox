#ifndef WB_SCRIPT_H
#define WB_SCRIPT_H

#include <cstdint>
#include <memory>

#include <optional>
#include <string>

#include "waybox/applications.hpp"
#include "waybox/geometry.hpp"
#include "waybox/menu.hpp"
#include "waybox/server.h"

/*
 * Lua scripting (script.cpp; built with -Dlua). Scripts are opt-in:
 * $XDG_CONFIG_HOME/waybox/init.lua if it exists, then each file in
 * <waybox><scripts><script>...</script></scripts>. Without any, none of
 * this costs anything. Errors are logged and skipped; a call that runs past
 * its time budget is stopped, and a callback that keeps doing so is turned
 * off. See waybox-lua(5).
 */
void init_scripts(struct wb_server *server);
void finish_scripts(struct wb_server *server);
/* After a Reconfigure: scripts are loaded afresh (deferred while one runs). */
void scripts_reload(struct wb_server *server);

/* Events, delivered to scripts once the change is over (on idle). */
void script_window_event(struct wb_toplevel *toplevel, const char *change);
void script_desktop_event(struct wb_server *server);
void script_output_event(struct wb_server *server, const char *name, const char *change);

/* Policy hooks (wb.policy), called while Waybox decides: a script gives a
 * decision, nil leaves it to Waybox. No actions run during them. */
struct ScriptWindow {
	std::string app_id, title, tag;
	int width = 0, height = 0;
	bool dialog = false, xwayland = false;
};
/* wb.policy("rules"): the keys it set, over rc.xml's rule. */
bool script_rules(struct wb_server *server, const ScriptWindow &w, wb::AppRule *out);
/* wb.policy("place"): where (and how big) the new window goes. */
std::optional<wb::Rect> script_place(struct wb_server *server, const ScriptWindow &w,
		const wb::Rect &area, const wb::Rect &proposed);
/* wb.policy("layout"): windows changed: lay them out again (on idle,
 * once for several changes). */
void script_request_layout(struct wb_server *server);

/* SIGCHLD: programs scripts started (wb.spawn) that have ended. */
void scripts_reap(struct wb_server *server);

/* Menus (wb.menu_provider, wb.show_menu). A menu being opened: the
 * provider's items for `id`, or nullptr if no script provides it. */
std::unique_ptr<wb::MenuFile> script_build_menu(struct wb_server *server, const std::string &id);
/* The label a script gave the menu `id` (wb.menu_provider), or "". */
std::string script_menu_label(struct wb_server *server, const std::string &id);
/* A menu entry a script gave a function was chosen. */
void script_menu_call(struct wb_server *server, int callback);
/* A new menu opens: drop the entry functions of the previous one. */
void script_menu_reset(struct wb_server *server);

/* A key a script bound (wb.bind): true if one ran. */
bool script_key_binding(struct wb_server *server, uint32_t sym, uint32_t modifiers);

#endif
