/*
 * Lua scripting: see script.h and waybox-lua(5).
 *
 * One Lua state, on the compositor's thread. Safety and cost:
 * - scripts run as the user, with the whole standard library;
 * - nothing runs unless a script is there; each event is a vector lookup;
 * - every call into Lua has a time budget (an instruction-count hook checks
 *   the clock); a call past it is stopped, and a callback stopped three
 *   times in a row is turned off;
 * - errors are logged with the script's file and line, and skipped;
 * - notification events are delivered on idle, after the change that
 *   caused them is over, so a script acting on a window can't pull it from
 *   under the code still handling it; windows are reached through handles
 *   that look the window up by id each time (nil once it's gone);
 * - a Reconfigure started from a script reloads scripts only once that
 *   script has returned.
 */
#include "script.h"

#include "waybox/wlroots.hpp"
#include "config.h"

#ifdef HAVE_LUA

#include <algorithm>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <set>
#include <unistd.h>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <lua.hpp>

#include "waybox/action.hpp"
#include "waybox/keychain.hpp"
#include "waybox/desktop_popup.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/style.hpp"
#include "waybox/session_lock.h"
#include "waybox/proc_group.hpp"
#include "waybox/stall.hpp"
#include "waybox/xdg_shell.h"

#if LUA_VERSION_NUM < 502
#define lua_rawlen lua_objlen
#endif

namespace {

constexpr int kHookInstructions = 1000;
constexpr int kStrikes = 3;           /* overruns in a row before a callback goes */
constexpr int kMaxDepth = 16;         /* scripts calling actions calling scripts */
const char *kTooLong = "waybox: took longer than its time budget";
const char *kWindowMeta = "waybox.window";

struct Event {
	std::string name;        /* "window.new", "desktop.switch", ... */
	std::string window_id;   /* window events */
	std::string app_id, title;
	int desktop = 0;
	std::string output;
	int skipped = 0;         /* bell */
};

struct Timer {
	int id = 0;
	int ref = LUA_NOREF;
	int period_ms = 0;       /* 0: once */
	struct wl_event_source *source = nullptr;
};

/* A program a script started (wb.spawn): its output, read as it comes,
 * and its end, then the script's callback. */
struct Spawn {
	pid_t pid = -1;
	int fd = -1;
	struct wl_event_source *source = nullptr;  /* its output */
	struct wl_event_source *grace = nullptr;   /* ended, output still open */
	std::string out;
	bool truncated = false, eof = false, exited = false;
	int status = 0;
	int ref = LUA_NOREF;                        /* the callback (or none) */
};

constexpr int kMaxSpawns = 16;
constexpr size_t kMaxOutput = 1 << 20;
/* Read per turn of the event loop: a program writing nonstop doesn't keep
 * the compositor from the rest (the next turn reads on). */
constexpr size_t kReadPerTurn = 64 * 1024;

/* Programs started and not yet reaped, whatever became of the script that
 * started them (a reload): reaped on SIGCHLD all the same. The limit on
 * running programs counts these, not only the current script's. */
std::vector<pid_t> unreaped;

/* An old script's programs still running when it was closed (a reload):
 * their process groups sent SIGTERM, then SIGKILL 2 s later; each leader
 * kept unreaped till then, so that its group's id is still its own. */
struct Stopping {
	pid_t pid;
	bool killed = false;
};
std::vector<Stopping> stopping;
struct wl_event_source *stop_timer = nullptr;

struct Binding {
	uint32_t sym = 0, modifiers = 0;
	int ref = LUA_NOREF;
};

struct State {
	struct wb_server *server = nullptr;
	lua_State *L = nullptr;
	std::map<std::string, std::vector<int>> handlers;
	std::vector<Binding> bindings;
	std::list<std::unique_ptr<Timer>> timers;
	std::list<std::unique_ptr<Spawn>> spawns;
	int next_timer = 1;
	std::map<int, int> strikes;
	std::deque<Event> queue;
	struct wl_event_source *idle = nullptr;
	struct wl_event_source *reload_idle = nullptr;
	int depth = 0;
	bool reload_pending = false;
	bool in_policy = false;            /* deciding: no actions */
	std::map<std::string, int> policies;
	struct wl_event_source *layout_idle = nullptr;
	double deadline = 0;
	int budget_ms = 10;
	std::map<std::string, int> menu_providers;  /* menu id -> function */
	std::map<std::string, std::string> menu_labels;  /* menu id -> label */
	std::map<int, int> menu_callbacks;          /* entry id -> function */
};

/* Menu entry ids go on across reloads, so an entry from before one finds
 * nothing rather than another script's function. */
int next_menu_callback = 1;

State *g = nullptr;

void close_state();
void open_state(struct wb_server *server);

/* ---- calling into Lua ---------------------------------------------- */

void budget_hook(lua_State *L, lua_Debug *) {
	if (g != nullptr && g->depth > 0 && wb::monotonic_ms() > g->deadline)
		luaL_error(L, "%s (%d ms)", kTooLong, g->budget_ms);
}

int traceback(lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
	return 1;
}

void forget(int ref);

void schedule_reload();

/* Call the function under `ref` with what `push` pushes; false on an error
 * (logged). The time budget runs from the outermost call. */
bool call(int ref, const std::function<int(lua_State *)> &push, const char *what) {
	if (g == nullptr || g->L == nullptr || ref == LUA_NOREF || g->depth >= kMaxDepth)
		return false;
	lua_State *L = g->L;
	wb::StallScope timing("script");
	lua_pushcfunction(L, traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	int n = push ? push(L) : 0;
	if (g->depth == 0)
		g->deadline = wb::monotonic_ms() + g->budget_ms;
	++g->depth;
	int rc = lua_pcall(L, n, 0, base);
	--g->depth;
	bool ok = rc == 0;
	if (!ok) {
		const char *msg = lua_tostring(L, -1);
		std::string m = msg ? msg : "(no message)";
		wlr_log(WLR_ERROR, "wb-script error in %s: %s", what, m.c_str());
		if (m.find(kTooLong) != std::string::npos && ++g->strikes[ref] >= kStrikes) {
			wlr_log(WLR_ERROR, "wb-script %s turned off: it kept running too long", what);
			forget(ref);
		}
		lua_pop(L, 1);
	} else {
		g->strikes.erase(ref);
	}
	lua_settop(L, base - 1);
	if (g->depth == 0 && g->reload_pending)
		schedule_reload();
	return ok;
}

/* As call(), with `nresults` results that `read` takes (before they go).
 * During it, actions are refused (a policy decides, it doesn't act). */
bool call_policy(int ref, const std::function<int(lua_State *)> &push, int nresults,
		const std::function<void(lua_State *)> &read, const char *what) {
	if (g == nullptr || g->L == nullptr || ref == LUA_NOREF || g->depth >= kMaxDepth)
		return false;
	lua_State *L = g->L;
	wb::StallScope timing("script-policy");
	lua_pushcfunction(L, traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	int n = push ? push(L) : 0;
	if (g->depth == 0)
		g->deadline = wb::monotonic_ms() + g->budget_ms;
	++g->depth;
	bool was = g->in_policy;
	g->in_policy = true;
	int rc = lua_pcall(L, n, nresults, base);
	g->in_policy = was;
	--g->depth;
	if (rc != 0) {
		const char *msg = lua_tostring(L, -1);
		std::string m = msg ? msg : "(no message)";
		wlr_log(WLR_ERROR, "wb-script error in %s: %s", what, m.c_str());
		if (m.find(kTooLong) != std::string::npos && ++g->strikes[ref] >= kStrikes) {
			wlr_log(WLR_ERROR, "wb-script %s turned off: it kept running too long", what);
			forget(ref);
		}
		lua_settop(L, base - 1);
		return false;
	}
	g->strikes.erase(ref);
	read(L);
	lua_settop(L, base - 1);
	return true;
}

/* A callback that's turned off: out of every list, and released. */
void forget(int ref) {
	for (auto it = g->policies.begin(); it != g->policies.end();)
		it = it->second == ref ? g->policies.erase(it) : std::next(it);
	for (auto &[name, refs] : g->handlers)
		refs.erase(std::remove(refs.begin(), refs.end(), ref), refs.end());
	for (auto it = g->menu_providers.begin(); it != g->menu_providers.end();) {
		if (it->second == ref) {
			g->menu_labels.erase(it->first);
			it = g->menu_providers.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = g->menu_callbacks.begin(); it != g->menu_callbacks.end();)
		it = it->second == ref ? g->menu_callbacks.erase(it) : std::next(it);
	g->bindings.erase(std::remove_if(g->bindings.begin(), g->bindings.end(),
			[ref](const Binding &b) { return b.ref == ref; }), g->bindings.end());
	for (auto it = g->timers.begin(); it != g->timers.end(); ++it)
		if ((*it)->ref == ref) {
			wl_event_source_remove((*it)->source);
			g->timers.erase(it);
			break;
		}
	g->strikes.erase(ref);
	luaL_unref(g->L, LUA_REGISTRYINDEX, ref);
}

/* ---- windows --------------------------------------------------------- */

struct wb_toplevel *window_by_id(const std::string &id) {
	struct wb_toplevel *t;
	wl_list_for_each(t, &g->server->toplevels, link)
		if (t->mapped && t->foreign_toplevel_handle != nullptr &&
				id == t->foreign_toplevel_handle->identifier)
			return t;
	return nullptr;
}

void push_window(lua_State *L, struct wb_toplevel *t) {
	if (t == nullptr || t->foreign_toplevel_handle == nullptr) {
		lua_pushnil(L);
		return;
	}
	const char *id = t->foreign_toplevel_handle->identifier;
	size_t len = strlen(id) + 1;
	auto *ud = static_cast<char *>(lua_newuserdata(L, len));
	memcpy(ud, id, len);
	luaL_getmetatable(L, kWindowMeta);
	lua_setmetatable(L, -2);
}

const char *window_id_arg(lua_State *L, int index) {
	return static_cast<const char *>(luaL_checkudata(L, index, kWindowMeta));
}

/* t[key] pushed, without metamethods (so nothing here can raise a Lua
 * error past C++ objects). */
void raw_field(lua_State *L, int t, const char *key) {
	lua_pushstring(L, key);
	lua_rawget(L, t);
}

std::string string_field(lua_State *L, int t, const char *key) {
	raw_field(L, t, key);
	std::string s;
	if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
		s = lua_tostring(L, -1);
	lua_pop(L, 1);
	return s;
}

/* An rc.xml action by name, with the options in the table at `params`
 * (0: none): command, to, direction, wrap, follow. */
std::optional<wb::Action> make_script_action(lua_State *L, const char *name, int params) {
	bool table = params > 0 && lua_istable(L, params);
	auto action = wb::make_action(name, table ? string_field(L, params, "command") : "");
	if (!action || !table)
		return action;
	if (std::string to = string_field(L, params, "to"); !to.empty())
		action->to = to;
	if (std::string d = string_field(L, params, "direction"); !d.empty())
		action->direction = d;
	raw_field(L, params, "wrap");
	if (lua_isboolean(L, -1))
		action->wrap = lua_toboolean(L, -1);
	lua_pop(L, 1);
	raw_field(L, params, "follow");
	if (lua_isboolean(L, -1))
		action->follow = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return action;
}

/* Run a named action (rc.xml's), on `target` or the focused window. */
bool run_named_action(lua_State *L, const char *name, int params, struct wb_toplevel *target) {
	auto action = make_script_action(L, name, params);
	if (!action)
		return false;
	struct wb_server *server = g->server;
	if (session_locked(server))
		return false;  /* as bindings: nothing behind the lock screen */
	if (g->in_policy) {
		wlr_log(WLR_ERROR, "wb-script: no actions in a policy (%s)", name);
		return false;
	}
	struct wb_toplevel *saved = server->action_window;
	server->action_window = target;
	wb::run_action(*action, server);
	server->action_window = saved;
	return true;
}

int window_action(lua_State *L) {
	std::string id = window_id_arg(L, 1);
	const char *name = luaL_checkstring(L, 2);
	struct wb_toplevel *t = window_by_id(id);
	lua_pushboolean(L, t != nullptr && run_named_action(L, name, lua_gettop(L) >= 3 ? 3 : 0, t));
	return 1;
}

int window_focus(lua_State *L) {
	struct wb_toplevel *t = window_by_id(window_id_arg(L, 1));
	if (t != nullptr && !session_locked(g->server) && !g->in_policy)
		focus_toplevel(t);
	lua_pushboolean(L, t != nullptr);
	return 1;
}

/* w:close(), w:raise(), ...: shortcuts for the actions of those names. */
int window_shortcut(lua_State *L) {
	const char *name = lua_tostring(L, lua_upvalueindex(1));
	struct wb_toplevel *t = window_by_id(window_id_arg(L, 1));
	lua_pushboolean(L, t != nullptr && run_named_action(L, name, 0, t));
	return 1;
}

int window_move_to_desktop(lua_State *L);
int window_set_geometry(lua_State *L);

int window_index(lua_State *L) {
	std::string id = window_id_arg(L, 1);
	const char *key = luaL_checkstring(L, 2);
	if (strcmp(key, "id") == 0) {
		lua_pushstring(L, id.c_str());
		return 1;
	}
	if (strcmp(key, "action") == 0) {
		lua_pushcfunction(L, window_action);
		return 1;
	}
	if (strcmp(key, "focus") == 0) {
		lua_pushcfunction(L, window_focus);
		return 1;
	}
	if (strcmp(key, "move_to_desktop") == 0) {
		lua_pushcfunction(L, window_move_to_desktop);
		return 1;
	}
	if (strcmp(key, "set_geometry") == 0) {
		lua_pushcfunction(L, window_set_geometry);
		return 1;
	}
	static const std::map<std::string, const char *> shortcuts = {
		{"close", "Close"}, {"raise", "Raise"}, {"lower", "Lower"},
		{"minimize", "Iconify"}, {"toggle_maximize", "ToggleMaximize"},
		{"toggle_fullscreen", "Fullscreen"}, {"toggle_decorations", "ToggleDecorations"},
		{"toggle_omnipresent", "ToggleOmnipresent"}, {"shade", "Shade"},
		{"unshade", "Unshade"},
	};
	if (auto s = shortcuts.find(key); s != shortcuts.end()) {
		lua_pushstring(L, s->second);
		lua_pushcclosure(L, window_shortcut, 1);
		return 1;
	}
	struct wb_toplevel *t = window_by_id(id);
	if (t == nullptr) {
		lua_pushnil(L);  /* the window is gone */
		return 1;
	}
	std::string k = key;
	if (k == "app_id")
		lua_pushstring(L, view_app_id(t) ? view_app_id(t) : "");
	else if (k == "title")
		lua_pushstring(L, view_title(t) ? view_title(t) : "");
	else if (k == "tag")
		lua_pushstring(L, t->tag.c_str());
	else if (k == "x")
		lua_pushinteger(L, t->geometry.x);
	else if (k == "y")
		lua_pushinteger(L, t->geometry.y);
	else if (k == "width")
		lua_pushinteger(L, t->geometry.width);
	else if (k == "height")
		lua_pushinteger(L, t->geometry.height);
	else if (k == "desktop")
		lua_pushinteger(L, t->desktop + 1);
	else if (k == "omnipresent")
		lua_pushboolean(L, t->omnipresent);
	else if (k == "minimized")
		lua_pushboolean(L, t->minimized);
	else if (k == "maximized")
		lua_pushboolean(L, t->max_horz && t->max_vert);
	else if (k == "fullscreen")
		lua_pushboolean(L, t->fullscreen);
	else if (k == "focused")
		lua_pushboolean(L, first_toplevel(g->server) == t);
	else if (k == "xwayland")
		lua_pushboolean(L, t->kind == wb_view_kind::Xwayland);
	else
		lua_pushnil(L);
	return 1;
}

/* w:move_to_desktop(n) */
int window_move_to_desktop(lua_State *L) {
	struct wb_toplevel *t = window_by_id(window_id_arg(L, 1));
	lua_Integer n = luaL_checkinteger(L, 2);
	lua_newtable(L);
	lua_pushstring(L, std::to_string(n).c_str());
	lua_setfield(L, -2, "to");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "follow");
	lua_pushboolean(L, t != nullptr && run_named_action(L, "SendToDesktop", lua_gettop(L), t));
	return 1;
}

/* w:set_geometry{x=, y=, width=, height=}: any of them; the window leaves
 * maximized/tiled first. Sizes are of the window's contents. */
int window_set_geometry(lua_State *L) {
	struct wb_toplevel *t = window_by_id(window_id_arg(L, 1));
	luaL_checktype(L, 2, LUA_TTABLE);
	if (t == nullptr || t->fullscreen || session_locked(g->server) || g->in_policy) {
		lua_pushboolean(L, 0);
		return 1;
	}
	if (t->tiled)
		untile_toplevel(t, false);
	if (t->max_horz || t->max_vert)
		set_toplevel_maximized(t, false, false);
	auto field = [L](const char *k, int fallback) {
		lua_getfield(L, 2, k);
		int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : fallback;
		lua_pop(L, 1);
		return v;
	};
	int w = field("width", t->geometry.width), h = field("height", t->geometry.height);
	t->geometry.x = std::clamp(field("x", t->geometry.x), -100000, 100000);
	t->geometry.y = std::clamp(field("y", t->geometry.y), -100000, 100000);
	if (w >= 1 && h >= 1 && (w != t->geometry.width || h != t->geometry.height)) {
		t->geometry.width = std::min(w, 32767);
		t->geometry.height = std::min(h, 32767);
		view_set_size(t, t->geometry.width, t->geometry.height);
	}
	position_toplevel(t);
	lua_pushboolean(L, 1);
	return 1;
}

int window_eq(lua_State *L) {
	lua_pushboolean(L, strcmp(window_id_arg(L, 1), window_id_arg(L, 2)) == 0);
	return 1;
}

int window_tostring(lua_State *L) {
	std::string id = window_id_arg(L, 1);
	struct wb_toplevel *t = window_by_id(id);
	lua_pushfstring(L, "window %s (%s)", id.c_str(),
			t != nullptr && view_app_id(t) ? view_app_id(t) : "gone");
	return 1;
}

/* ---- the wb module ---------------------------------------------------- */

int wb_on(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	static const std::set<std::string> known = {"window.new", "window.close",
		"window.focus", "window.title", "desktop.switch", "output.added", "output.removed",
		"bell"};
	if (known.count(name) == 0)
		return luaL_error(L, "unknown event: %s", name);
	lua_pushvalue(L, 2);
	g->handlers[name].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
	return 0;
}

int wb_windows(lua_State *L) {
	lua_newtable(L);
	int i = 1;
	struct wb_toplevel *t;
	wl_list_for_each(t, &g->server->focus_order, focus_link) {
		if (!t->mapped || t->foreign_toplevel_handle == nullptr)
			continue;
		push_window(L, t);
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

int wb_focused(lua_State *L) {
	push_window(L, first_toplevel(g->server));
	return 1;
}

int wb_desktop(lua_State *L) {
	lua_newtable(L);
	/* The desktop of the monitor desktop actions act on (the focused
	 * window's, else the pointer's): with shared desktops, the one shown. */
	int cur = 0, last = 0;
	desktop_action_state(g->server, &cur, &last);
	lua_pushinteger(L, cur + 1);
	lua_setfield(L, -2, "current");
	lua_pushinteger(L, desktop_count(g->server));
	lua_setfield(L, -2, "count");
	/* monitors = { ["DP-1"] = 2, ... }: each monitor's (#367). */
	lua_newtable(L);
	struct wb_output *o;
	wl_list_for_each(o, &g->server->outputs, link) {
		if (!o->wlr_output->enabled)
			continue;
		lua_pushinteger(L, output_desktop(g->server, o->wlr_output) + 1);
		lua_setfield(L, -2, o->wlr_output->name);
	}
	lua_setfield(L, -2, "monitors");
	return 1;
}

int wb_outputs(lua_State *L) {
	lua_newtable(L);
	int i = 1;
	struct wb_output *o;
	wl_list_for_each(o, &g->server->outputs, link) {
		lua_newtable(L);
		lua_pushstring(L, o->wlr_output->name);
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, o->geometry.x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, o->geometry.y);
		lua_setfield(L, -2, "y");
		lua_pushinteger(L, o->geometry.width);
		lua_setfield(L, -2, "width");
		lua_pushinteger(L, o->geometry.height);
		lua_setfield(L, -2, "height");
		lua_pushnumber(L, o->wlr_output->scale);
		lua_setfield(L, -2, "scale");
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

int wb_action(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	struct wb_toplevel *target = nullptr;
	if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
		target = window_by_id(window_id_arg(L, 3));
		if (target == nullptr) {
			lua_pushboolean(L, 0);
			return 1;
		}
	}
	lua_pushboolean(L, run_named_action(L, name, lua_gettop(L) >= 2 ? 2 : 0, target));
	return 1;
}

int wb_bind(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	Binding b;
	wb_parse_key_combo(key, &b.sym, &b.modifiers);
	if (b.sym == 0)
		return luaL_error(L, "not a key: %s", key);
	lua_pushvalue(L, 2);
	b.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	g->bindings.push_back(b);
	return 0;
}

int on_timer(void *data) {
	auto *timer = static_cast<Timer *>(data);
	int id = timer->id, ref = timer->ref;
	call(ref, nullptr, "a timer");
	/* (It may have been cancelled, or turned off, meanwhile.) */
	for (auto it = g->timers.begin(); it != g->timers.end(); ++it) {
		if ((*it)->id != id)
			continue;
		if ((*it)->period_ms > 0) {
			wl_event_source_timer_update((*it)->source, (*it)->period_ms);
		} else {
			wl_event_source_remove((*it)->source);
			luaL_unref(g->L, LUA_REGISTRYINDEX, (*it)->ref);
			g->timers.erase(it);
		}
		break;
	}
	return 0;
}

int add_timer(lua_State *L, bool repeat) {
	lua_Integer ms = luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (ms < (repeat ? 10 : 0))
		return luaL_error(L, "a %s needs at least %d ms", repeat ? "repeating timer" : "timer",
				repeat ? 10 : 0);
	auto timer = std::make_unique<Timer>();
	timer->id = g->next_timer++;
	timer->period_ms = repeat ? static_cast<int>(ms) : 0;
	lua_pushvalue(L, 2);
	timer->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	timer->source = wl_event_loop_add_timer(g->server->wl_event_loop, on_timer, timer.get());
	if (timer->source == nullptr) {
		luaL_unref(L, LUA_REGISTRYINDEX, timer->ref);
		return luaL_error(L, "no timer");
	}
	wl_event_source_timer_update(timer->source, ms > 0 ? static_cast<int>(ms) : 1);
	lua_pushinteger(L, timer->id);
	g->timers.push_back(std::move(timer));
	return 1;
}

int wb_timer(lua_State *L) { return add_timer(L, false); }
int wb_every(lua_State *L) { return add_timer(L, true); }

int wb_cancel(lua_State *L) {
	lua_Integer id = luaL_checkinteger(L, 1);
	for (auto it = g->timers.begin(); it != g->timers.end(); ++it)
		if ((*it)->id == id) {
			wl_event_source_remove((*it)->source);
			luaL_unref(L, LUA_REGISTRYINDEX, (*it)->ref);
			g->timers.erase(it);
			break;
		}
	return 0;
}

std::string concat_args(lua_State *L) {
	std::string out;
	int n = lua_gettop(L);
	for (int i = 1; i <= n; ++i) {
		if (i > 1)
			out += " ";
		lua_getglobal(L, "tostring");
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		const char *s = lua_tostring(L, -1);
		out += s ? s : "";
		lua_pop(L, 1);
	}
	return out;
}

int wb_log(lua_State *L) {
	wlr_log(WLR_INFO, "wb-script: %s", concat_args(L).c_str());
	return 0;
}

void spawn_drop(Spawn *s) {
	if (s->source != nullptr)
		wl_event_source_remove(s->source);
	if (s->grace != nullptr)
		wl_event_source_remove(s->grace);
	if (s->fd >= 0)
		close(s->fd);
	if (s->ref != LUA_NOREF && g != nullptr && g->L != nullptr)
		luaL_unref(g->L, LUA_REGISTRYINDEX, s->ref);
	if (g != nullptr)
		g->spawns.remove_if([s](const auto &p) { return p.get() == s; });
}

/* Ended and its output read (or the grace time over): the callback. */
void spawn_finish(Spawn *s) {
	int ref = s->ref;
	s->ref = LUA_NOREF;
	std::string out = std::move(s->out);
	bool truncated = s->truncated;
	int code = WIFEXITED(s->status) ? WEXITSTATUS(s->status)
			: WIFSIGNALED(s->status) ? -WTERMSIG(s->status) : -1;
	spawn_drop(s);
	if (ref != LUA_NOREF) {
		call(ref, [&](lua_State *L) {
			lua_pushlstring(L, out.data(), out.size());
			lua_pushinteger(L, code);
			lua_pushboolean(L, truncated);
			return 3;
		}, "a spawn callback");
		if (g != nullptr && g->L != nullptr)
			luaL_unref(g->L, LUA_REGISTRYINDEX, ref);
	}
}

int spawn_readable(int fd, uint32_t mask, void *data) {
	wb::StallScope timing("spawn-read");
	auto *s = static_cast<Spawn *>(data);
	char buf[8192];
	for (size_t turn = 0;;) {
		if (turn >= kReadPerTurn)
			return 0;  /* more next turn (still readable: called again) */
		ssize_t n = read(fd, buf, sizeof buf);
		if (n > 0) {
			turn += static_cast<size_t>(n);
			size_t room = kMaxOutput - std::min(kMaxOutput, s->out.size());
			s->out.append(buf, std::min(room, static_cast<size_t>(n)));
			if (static_cast<size_t>(n) > room)
				s->truncated = true;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(mask & WL_EVENT_HANGUP))
			return 0;
		break;  /* end of output (or an error) */
	}
	wl_event_source_remove(s->source);
	s->source = nullptr;
	close(s->fd);
	s->fd = -1;
	s->eof = true;
	if (s->exited)
		spawn_finish(s);
	return 0;
}

/* wb.spawn(command, callback): command is a string (run by sh) or a list
 * (argv); the callback gets (output, exit code, truncated). Never waits. */
int wb_spawn(lua_State *L) {
	if (!lua_isstring(L, 1) && !lua_istable(L, 1))
		return luaL_error(L, "wb.spawn: a command string or a list of arguments");
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
		luaL_checktype(L, 2, LUA_TFUNCTION);
	if (static_cast<int>(unreaped.size()) >= kMaxSpawns) {
		lua_pushnil(L);
		lua_pushstring(L, "too many programs still running");
		return 2;
	}
	std::vector<std::string> argv;
	if (lua_isstring(L, 1)) {
		argv = {"/bin/sh", "-c", lua_tostring(L, 1)};
	} else {
		for (int i = 1; i <= static_cast<int>(lua_rawlen(L, 1)); ++i) {
			lua_rawgeti(L, 1, i);
			if (!lua_isstring(L, -1))
				return luaL_error(L, "wb.spawn: argument %d isn't a string", i);
			argv.emplace_back(lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		if (argv.empty())
			return luaL_error(L, "wb.spawn: an empty command");
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0)
		return luaL_error(L, "wb.spawn: %s", strerror(errno));
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return luaL_error(L, "wb.spawn: %s", strerror(errno));
	}
	if (pid == 0) {
		/* Its own process group: stopped with what it started. */
		setpgid(0, 0);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, 0);
		dup2(fds[1], 1);
		std::vector<char *> args;
		for (std::string &a : argv)
			args.push_back(a.data());
		args.push_back(nullptr);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(fds[1]);
	setpgid(pid, pid);  /* also on this side: a stop before it ran reaches it */
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	unreaped.push_back(pid);
	auto s = std::make_unique<Spawn>();
	s->pid = pid;
	s->fd = fds[0];
	if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
		lua_pushvalue(L, 2);
		s->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	s->source = wl_event_loop_add_fd(g->server->wl_event_loop, s->fd,
			WL_EVENT_READABLE, spawn_readable, s.get());
	g->spawns.push_back(std::move(s));
	lua_pushinteger(L, pid);
	return 1;
}

/* wb.policy(name, fn): "place", "rules" or "layout"; nil removes it. */
int wb_policy(lua_State *L) {
	std::string name = luaL_checkstring(L, 1);
	if (name != "place" && name != "rules" && name != "layout" && name != "bell")
		return luaL_error(L, "unknown policy: %s", name.c_str());
	if (auto old = g->policies.find(name); old != g->policies.end()) {
		luaL_unref(L, LUA_REGISTRYINDEX, old->second);
		g->policies.erase(old);
	}
	if (!lua_isnil(L, 2)) {
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushvalue(L, 2);
		g->policies[name] = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	if (name == "layout")
		script_request_layout(g->server);
	return 0;
}

int wb_relayout(lua_State *) {
	script_request_layout(g->server);
	return 0;
}

/* wb.go_to_desktop(n) */
int wb_go_to_desktop(lua_State *L) {
	lua_Integer n = luaL_checkinteger(L, 1);
	lua_newtable(L);
	lua_pushstring(L, std::to_string(n).c_str());
	lua_setfield(L, -2, "to");
	lua_pushboolean(L, run_named_action(L, "GoToDesktop", lua_gettop(L), nullptr));
	return 1;
}

/* ---- menus and the on-screen display ---------------------------------- */

constexpr const char *kScriptMenuId = "wb.show_menu";
constexpr size_t kMaxMenuItems = 500;
constexpr int kMaxMenuDepth = 8;

/* The action of the entry whose table is at `item`: a function, an action
 * name, or {name, command=..., to=...}. */
void entry_action(lua_State *L, int item, wb::MenuItem &out) {
	raw_field(L, item, "action");
	int a = lua_gettop(L);
	if (lua_isfunction(L, a)) {
		lua_pushvalue(L, a);
		int ref = luaL_ref(L, LUA_REGISTRYINDEX);
		int id = next_menu_callback++;
		g->menu_callbacks[id] = ref;
		wb::Action call{wb::ActionType::ScriptCall};
		call.script_callback = id;
		out.actions.push_back(call);
	} else if (lua_type(L, a) == LUA_TSTRING) {
		if (auto action = make_script_action(L, lua_tostring(L, a), 0))
			out.actions.push_back(*action);
		else
			wlr_log(WLR_ERROR, "wb-script menu: no action %s", lua_tostring(L, a));
	} else if (lua_istable(L, a)) {
		lua_rawgeti(L, a, 1);
		std::string name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		if (auto action = make_script_action(L, name.c_str(), a))
			out.actions.push_back(*action);
		else
			wlr_log(WLR_ERROR, "wb-script menu: no action %s", name.c_str());
	}
	lua_pop(L, 1);
}

/* The items in the array at `t` into `menu`; nested submenus go into
 * `file` as <menu id>/<n>. */
void build_items(lua_State *L, int t, wb::Menu &menu, wb::MenuFile &file, int &serial,
		int depth) {
	if (!lua_checkstack(L, 8))
		return;
	size_t n = lua_rawlen(L, t);
	for (size_t i = 1; i <= n && menu.items.size() < kMaxMenuItems; i++) {
		lua_rawgeti(L, t, static_cast<int>(i));
		int it = lua_gettop(L);
		if (!lua_istable(L, it)) {
			lua_pop(L, 1);
			continue;
		}
		wb::MenuItem item;
		item.label = string_field(L, it, "label");
		item.icon = string_field(L, it, "icon");
		raw_field(L, it, "separator");
		bool separator = lua_toboolean(L, -1);
		lua_pop(L, 1);
		raw_field(L, it, "submenu");
		int sub = lua_gettop(L);
		if (separator) {
			item.kind = wb::MenuItem::Kind::Separator;
		} else if (lua_istable(L, sub) && depth < kMaxMenuDepth) {
			wb::Menu child;
			child.id = menu.id + "/" + std::to_string(serial++);
			child.label = item.label;
			build_items(L, sub, child, file, serial, depth + 1);
			item.kind = wb::MenuItem::Kind::Submenu;
			item.submenu_id = child.id;
			file.menus.push_back(std::move(child));
		} else if (lua_type(L, sub) == LUA_TSTRING) {
			/* Another menu by id: menu.xml's or a provider's. */
			item.kind = wb::MenuItem::Kind::Submenu;
			item.submenu_id = lua_tostring(L, sub);
		} else {
			item.kind = wb::MenuItem::Kind::Entry;
			entry_action(L, it, item);
		}
		lua_pop(L, 2);  /* submenu, item */
		menu.items.push_back(std::move(item));
	}
}

/* A menu with only a note in it (a provider that failed or gave nothing). */
void note_only(wb::Menu &menu, const char *note) {
	wb::MenuItem item;
	item.kind = wb::MenuItem::Kind::Separator;
	item.label = note;
	menu.items.push_back(std::move(item));
}

/* wb.menu_provider(id, fn, label): the menu `id` is fn's items, built
 * each time it opens; nil removes it. */
int wb_menu_provider(lua_State *L) {
	luaL_checkstring(L, 1);
	if (!lua_isnil(L, 2))
		luaL_checktype(L, 2, LUA_TFUNCTION);
	luaL_optstring(L, 3, "");
	std::string id = lua_tostring(L, 1);
	g->menu_labels.erase(id);
	if (!lua_isnil(L, 2) && lua_type(L, 3) == LUA_TSTRING)
		g->menu_labels[id] = lua_tostring(L, 3);
	if (auto old = g->menu_providers.find(id); old != g->menu_providers.end()) {
		luaL_unref(L, LUA_REGISTRYINDEX, old->second);
		g->menu_providers.erase(old);
	}
	if (!lua_isnil(L, 2)) {
		lua_pushvalue(L, 2);
		g->menu_providers[id] = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	return 0;
}

/* wb.show_menu(items, {x=, y=, window=}): open a menu (at the pointer by
 * default); with `window`, its rc.xml actions act on that window. */
int wb_show_menu(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	struct wb_server *server = g->server;
	struct wb_toplevel *target = nullptr;
	int x = static_cast<int>(server->cursor->cursor->x);
	int y = static_cast<int>(server->cursor->cursor->y);
	if (lua_istable(L, 2)) {
		raw_field(L, 2, "window");
		if (!lua_isnil(L, -1)) {
			target = window_by_id(window_id_arg(L, -1));
			if (target == nullptr) {
				lua_pushboolean(L, 0);
				return 1;
			}
		}
		lua_pop(L, 1);
		raw_field(L, 2, "x");
		if (lua_isnumber(L, -1))
			x = static_cast<int>(lua_tointeger(L, -1));
		lua_pop(L, 1);
		raw_field(L, 2, "y");
		if (lua_isnumber(L, -1))
			y = static_cast<int>(lua_tointeger(L, -1));
		lua_pop(L, 1);
	}
	if (g->in_policy) {
		wlr_log(WLR_ERROR, "%s", "wb-script: no menus in a policy");
		lua_pushboolean(L, 0);
		return 1;
	}
	if (session_locked(server) || server->config == nullptr) {
		lua_pushboolean(L, 0);
		return 1;
	}
	bool ok = false;
	{
		server->menu.reset();  /* it may show the previous script menu */
		server->menu_target = nullptr;
		server->script_menu = std::make_unique<wb::MenuFile>();
		wb::StallScope timing("menu");
		/* (Its constructor drops the previous menu's entry functions:
		 * build this one's after it.) */
		auto widget = std::make_unique<wb::MenuWidget>(server, *server->script_menu,
				wb::menu_style_from_theme(server->config->theme),
				server->config->menu_behavior);
		wb::Menu root;
		root.id = kScriptMenuId;
		int serial = 1;
		build_items(L, 1, root, *server->script_menu, serial, 0);
		if (!root.items.empty()) {
			server->script_menu->menus.push_back(std::move(root));
			ok = widget->open(kScriptMenuId, x, y);
		}
		if (ok) {
			server->menu = std::move(widget);
			server->preview_menu.reset();  /* (the old widget, gone, may have shown it) */
			server->menu_target = target;
		}
	}
	lua_pushboolean(L, ok);
	return 1;
}

/* wb.osd(text[, ms]): show text for a moment, as the desktop name shows;
 * "" hides it. */
int wb_osd(lua_State *L) {
	luaL_checkstring(L, 1);
	lua_Integer ms = luaL_optinteger(L, 2, 1500);
	if (g->in_policy) {
		wlr_log(WLR_ERROR, "%s", "wb-script: no on-screen display in a policy");
		return 0;
	}
	struct wb_server *server = g->server;
	std::string text = lua_tostring(L, 1);
	if (text.empty()) {
		if (server->desktop_popup != nullptr)
			server->desktop_popup->hide();
		return 0;
	}
	if (ms <= 0)
		ms = 1500;
	wb_show_popup(server, text, static_cast<int>(std::min<lua_Integer>(ms, 60000)));
	return 0;
}

const luaL_Reg wb_functions[] = {
	{"on", wb_on}, {"windows", wb_windows}, {"focused", wb_focused},
	{"desktop", wb_desktop}, {"outputs", wb_outputs}, {"action", wb_action},
	{"bind", wb_bind}, {"timer", wb_timer}, {"every", wb_every}, {"cancel", wb_cancel},
	{"log", wb_log}, {"spawn", wb_spawn}, {"go_to_desktop", wb_go_to_desktop},
	{"policy", wb_policy}, {"relayout", wb_relayout},
	{"menu_provider", wb_menu_provider}, {"show_menu", wb_show_menu}, {"osd", wb_osd},
	{nullptr, nullptr},
};

/* ---- the state ---------------------------------------------------------- */

void open_lib(lua_State *L, const char *name, lua_CFunction f) {
#if LUA_VERSION_NUM >= 502
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
#else
	lua_pushcfunction(L, f);
	lua_pushstring(L, name);
	lua_call(L, 1, 0);
#endif
}

/* The whole standard library: a script runs as you, like your autostart
 * script (Execute runs any command anyway, so holding back io or os would
 * protect nothing). What protects the session is the time budget. */
void open_libraries(lua_State *L) {
#if LUA_VERSION_NUM >= 502
	open_lib(L, "_G", luaopen_base);
#else
	open_lib(L, "", luaopen_base);
#endif
	open_lib(L, LUA_TABLIBNAME, luaopen_table);
	open_lib(L, LUA_STRLIBNAME, luaopen_string);
	open_lib(L, LUA_MATHLIBNAME, luaopen_math);
#if LUA_VERSION_NUM >= 503
	open_lib(L, LUA_UTF8LIBNAME, luaopen_utf8);
#endif
	open_lib(L, LUA_OSLIBNAME, luaopen_os);
	open_lib(L, LUA_IOLIBNAME, luaopen_io);
	open_lib(L, LUA_LOADLIBNAME, luaopen_package);
	open_lib(L, LUA_DBLIBNAME, luaopen_debug);
#ifdef LUA_BITLIBNAME
	open_lib(L, LUA_BITLIBNAME, luaopen_bit);  /* LuaJIT */
#endif
	/* Not LuaJIT's "jit" library: opening it starts the JIT compiler, and
	 * compiled code never runs the hook that enforces the time budget (an
	 * endless loop would freeze Waybox). Its interpreter is fast enough
	 * for event handlers and layouts. */
}

int print_to_log(lua_State *L) {
	wlr_log(WLR_INFO, "wb-script: %s", concat_args(L).c_str());
	return 0;
}

void load_file(const std::string &path) {
	lua_State *L = g->L;
	if (luaL_loadfile(L, path.c_str()) != 0) {
		wlr_log(WLR_ERROR, "wb-script %s: %s", path.c_str(), lua_tostring(L, -1));
		lua_pop(L, 1);
		return;
	}
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	call(ref, nullptr, path.c_str());
	luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

std::vector<std::string> script_paths(struct wb_server *server) {
	std::vector<std::string> out;
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	std::string config = xdg && *xdg ? xdg : std::string(home ? home : "") + "/.config";
	std::string init = config + "/waybox/init.lua";
	if (access(init.c_str(), R_OK) == 0)
		out.push_back(init);
	if (server->config != nullptr)
		for (std::string p : server->config->scripts) {
			if (p.rfind("~/", 0) == 0 && home)
				p = std::string(home) + p.substr(1);
			else if (!p.empty() && p[0] != '/')
				p = config + "/waybox/" + p;
			out.push_back(p);
		}
	return out;
}

void open_state(struct wb_server *server) {
	std::vector<std::string> paths = script_paths(server);
	if (paths.empty())
		return;  /* no scripts: nothing at all */
	g = new State;
	g->server = server;
	if (server->config != nullptr)
		g->budget_ms = server->config->script_budget_ms;
	g->L = luaL_newstate();
	if (g->L == nullptr) {
		wlr_log(WLR_ERROR, "%s", "wb-script: no Lua state");
		delete g;
		g = nullptr;
		return;
	}
	lua_State *L = g->L;
	open_libraries(L);
	lua_pushcfunction(L, print_to_log);
	lua_setglobal(L, "print");
	luaL_newmetatable(L, kWindowMeta);
	lua_pushcfunction(L, window_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, window_eq);
	lua_setfield(L, -2, "__eq");
	lua_pushcfunction(L, window_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);
	lua_newtable(L);
	luaL_setfuncs(L, wb_functions, 0);
	lua_pushinteger(L, 1);
	lua_setfield(L, -2, "api_version");
	lua_setglobal(L, "wb");
	lua_sethook(L, budget_hook, LUA_MASKCOUNT, kHookInstructions);
	for (const std::string &p : paths)
		load_file(p);
	wlr_log(WLR_INFO, "wb-scripts loaded: %zu file(s)", paths.size());
}

/* A leader (not reaped) that has exited: looked at, left unreaped. */
bool leader_exited(pid_t pid) {
	siginfo_t si;
	memset(&si, 0, sizeof si);
	return waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
			si.si_pid == pid;
}

void reap_leader(pid_t pid) {
	while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
	}
	unreaped.erase(std::remove(unreaped.begin(), unreaped.end(), pid), unreaped.end());
}

int stop_due(void *) {
	bool again = false;
	for (auto it = stopping.begin(); it != stopping.end();) {
		if (!it->killed) {
			if (kill(-it->pid, SIGKILL) != 0)
				kill(it->pid, SIGKILL);
			it->killed = true;
		}
		if (leader_exited(it->pid)) {
			reap_leader(it->pid);
			it = stopping.erase(it);
		} else {
			again = true;  /* killed, not exited yet */
			++it;
		}
	}
	if (again) {
		wl_event_source_timer_update(stop_timer, 200);
	} else {
		wl_event_source_remove(stop_timer);
		stop_timer = nullptr;
	}
	return 0;
}

void stop_spawned(pid_t pid) {
	if (kill(-pid, SIGTERM) != 0)
		kill(pid, SIGTERM);
	stopping.push_back({pid});
	if (stop_timer == nullptr && g != nullptr) {
		stop_timer = wl_event_loop_add_timer(g->server->wl_event_loop, stop_due, g->server);
		wl_event_source_timer_update(stop_timer, 2000);
	}
}

void close_state() {
	if (g == nullptr)
		return;
	for (auto &t : g->timers)
		wl_event_source_remove(t->source);
	g->timers.clear();
	/* Its programs still running go with it (the script, loaded again,
	 * starts what it wants again). */
	for (auto &sp : g->spawns)
		if (!sp->exited)
			stop_spawned(sp->pid);
	while (!g->spawns.empty())
		spawn_drop(g->spawns.front().get());
	if (g->idle != nullptr)
		wl_event_source_remove(g->idle);
	if (g->reload_idle != nullptr)
		wl_event_source_remove(g->reload_idle);
	if (g->layout_idle != nullptr)
		wl_event_source_remove(g->layout_idle);
	if (g->L != nullptr)
		lua_close(g->L);
	delete g;
	g = nullptr;
}

/* ---- events ----------------------------------------------------------- */

int dispatch(void *) {
	if (g == nullptr)
		return 0;
	g->idle = nullptr;
	std::deque<Event> events;
	events.swap(g->queue);
	for (const Event &e : events) {
		if (g == nullptr)
			break;  /* (a reload can't happen mid-dispatch, but be sure) */
		auto h = g->handlers.find(e.name);
		if (h == g->handlers.end())
			continue;
		std::vector<int> refs = h->second;  /* a handler may add or drop handlers */
		for (int ref : refs) {
			call(ref, [&e](lua_State *L) {
				if (e.name.rfind("window.", 0) == 0 || e.name == "bell") {
					/* A bell from no window: nil, and a table of "". */
					struct wb_toplevel *t = e.window_id.empty() ? nullptr
							: window_by_id(e.window_id);
					if (t != nullptr) {
						push_window(L, t);
					} else if (e.window_id.empty()) {
						lua_pushnil(L);
					} else {
						/* Gone: a handle that reads nil (its id kept). */
						size_t len = e.window_id.size() + 1;
						auto *ud = static_cast<char *>(lua_newuserdata(L, len));
						memcpy(ud, e.window_id.c_str(), len);
						luaL_getmetatable(L, kWindowMeta);
						lua_setmetatable(L, -2);
					}
					lua_newtable(L);
					lua_pushstring(L, e.window_id.c_str());
					lua_setfield(L, -2, "id");
					lua_pushstring(L, e.app_id.c_str());
					lua_setfield(L, -2, "app_id");
					lua_pushstring(L, e.title.c_str());
					lua_setfield(L, -2, "title");
					if (e.name == "bell") {
						lua_pushinteger(L, e.skipped);
						lua_setfield(L, -2, "skipped");
					}
					return 2;
				}
				if (e.name == "desktop.switch") {
					lua_pushinteger(L, e.desktop);
					return 1;
				}
				lua_pushstring(L, e.output.c_str());
				return 1;
			}, e.name.c_str());
		}
	}
	return 0;
}

void enqueue(Event e) {
	if (g == nullptr || g->handlers.count(e.name) == 0 || g->handlers[e.name].empty())
		return;
	g->queue.push_back(std::move(e));
	if (g->idle == nullptr)
		g->idle = wl_event_loop_add_idle(g->server->wl_event_loop,
				[](void *d) { dispatch(d); }, nullptr);
}

void schedule_reload() {
	if (g == nullptr || g->reload_idle != nullptr)
		return;
	g->reload_idle = wl_event_loop_add_idle(g->server->wl_event_loop, [](void *data) {
		auto *server = static_cast<struct wb_server *>(data);
		g->reload_idle = nullptr;
		close_state();
		open_state(server);
	}, g->server);
}

}  // namespace

void init_scripts(struct wb_server *server) {
	open_state(server);
}

void finish_scripts(struct wb_server *) {
	close_state();
	/* Exiting: no timer runs now. Up to 0.5 s for what was sent SIGTERM,
	 * then SIGKILL to what's left (each leader still unreaped, its group's
	 * id its own), reaped once it has exited. */
	for (int i = 0; i < 25 && !stopping.empty(); ++i) {
		scripts_reap(nullptr);
		if (!stopping.empty())
			usleep(20 * 1000);
	}
	for (Stopping &st : stopping)
		if (kill(-st.pid, SIGKILL) != 0)
			kill(st.pid, SIGKILL);
	for (int i = 0; i < 25 && !stopping.empty(); ++i) {
		stopping.erase(std::remove_if(stopping.begin(), stopping.end(), [](const Stopping &st) {
			if (!leader_exited(st.pid))
				return false;
			reap_leader(st.pid);
			return true;
		}), stopping.end());
		if (!stopping.empty())
			usleep(20 * 1000);
	}
	if (stop_timer != nullptr) {
		wl_event_source_remove(stop_timer);
		stop_timer = nullptr;
	}
}

void scripts_reload(struct wb_server *server) {
	if (g != nullptr && g->depth > 0) {
		g->reload_pending = true;  /* once the running script returns */
		return;
	}
	close_state();
	open_state(server);
}

void script_window_event(struct wb_toplevel *t, const char *change) {
	if (g == nullptr || t->foreign_toplevel_handle == nullptr)
		return;
	if (strcmp(change, "new") == 0 || strcmp(change, "close") == 0)
		script_request_layout(t->server);
	static const std::map<std::string, std::string> names = {
		{"new", "window.new"}, {"close", "window.close"}, {"focus", "window.focus"},
		{"title", "window.title"},
	};
	auto n = names.find(change);
	if (n == names.end())
		return;
	Event e;
	e.name = n->second;
	e.window_id = t->foreign_toplevel_handle->identifier;
	e.app_id = view_app_id(t) ? view_app_id(t) : "";
	e.title = view_title(t) ? view_title(t) : "";
	enqueue(std::move(e));
}

void script_bell_event(struct wb_server *, struct wb_toplevel *t, int skipped) {
	if (g == nullptr)
		return;
	Event e;
	e.name = "bell";
	e.skipped = skipped;
	if (t != nullptr && t->foreign_toplevel_handle != nullptr) {
		e.window_id = t->foreign_toplevel_handle->identifier;
		e.app_id = view_app_id(t) ? view_app_id(t) : "";
		e.title = view_title(t) ? view_title(t) : "";
	}
	enqueue(std::move(e));
}

void script_desktop_event(struct wb_server *server) {
	if (g == nullptr)
		return;
	script_request_layout(server);
	Event e;
	e.name = "desktop.switch";
	e.desktop = server->current_desktop + 1;
	enqueue(std::move(e));
}

void script_output_event(struct wb_server *server, const char *name, const char *change) {
	if (g == nullptr)
		return;
	script_request_layout(server);
	Event e;
	e.name = std::string("output.") + change;
	e.output = name;
	enqueue(std::move(e));
}

namespace {

void push_window_table(lua_State *L, const ScriptWindow &w) {
	lua_newtable(L);
	lua_pushstring(L, w.app_id.c_str());
	lua_setfield(L, -2, "app_id");
	lua_pushstring(L, w.title.c_str());
	lua_setfield(L, -2, "title");
	lua_pushstring(L, w.tag.c_str());
	lua_setfield(L, -2, "tag");
	lua_pushinteger(L, w.width);
	lua_setfield(L, -2, "width");
	lua_pushinteger(L, w.height);
	lua_setfield(L, -2, "height");
	lua_pushboolean(L, w.dialog);
	lua_setfield(L, -2, "dialog");
	lua_pushboolean(L, w.xwayland);
	lua_setfield(L, -2, "xwayland");
}

void push_rect(lua_State *L, const wb::Rect &r) {
	lua_newtable(L);
	lua_pushinteger(L, r.x);
	lua_setfield(L, -2, "x");
	lua_pushinteger(L, r.y);
	lua_setfield(L, -2, "y");
	lua_pushinteger(L, r.width);
	lua_setfield(L, -2, "width");
	lua_pushinteger(L, r.height);
	lua_setfield(L, -2, "height");
}

std::optional<int> int_field(lua_State *L, int t, const char *k) {
	lua_getfield(L, t, k);
	std::optional<int> v;
	if (lua_isnumber(L, -1))
		v = static_cast<int>(std::clamp<lua_Number>(lua_tonumber(L, -1), -100000, 100000));
	lua_pop(L, 1);
	return v;
}

std::optional<bool> bool_field(lua_State *L, int t, const char *k) {
	lua_getfield(L, t, k);
	std::optional<bool> v;
	if (lua_isboolean(L, -1))
		v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

}  // namespace

bool script_bell(struct wb_server *, struct wb_toplevel *t, ScriptBell *out) {
	if (g == nullptr || g->policies.count("bell") == 0)
		return false;
	bool got = false;
	call_policy(g->policies["bell"], [t](lua_State *L) {
		push_window(L, t);
		return 1;
	}, 1, [&](lua_State *L) {
		if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
			got = true;
			out->silent = true;
			return;
		}
		if (!lua_istable(L, -1))
			return;  /* nil (or anything else): rc.xml decides */
		got = true;
		int tbl = lua_gettop(L);
		for (const char *k : {"flash", "color", "duration", "count", "width"}) {
			lua_getfield(L, tbl, k);
			if (lua_type(L, -1) == LUA_TNUMBER) {
				/* 200, not "200.0" (Lua 5.1 has only doubles); out of
				 * range: clamped later. */
				double d = lua_tonumber(L, -1);
				out->over[k] = d == d ? std::to_string(static_cast<long>(
						std::clamp(d, -1e9, 1e9))) : "x";
			} else if (lua_type(L, -1) == LUA_TSTRING) {
				size_t len = 0;
				const char *v = lua_tolstring(L, -1, &len);
				out->over[k] = std::string(v, std::min<size_t>(len, 64));
			} else if (lua_type(L, -1) == LUA_TBOOLEAN && strcmp(k, "flash") == 0) {
				out->over[k] = lua_toboolean(L, -1) ? "yes" : "no";
			}
			lua_pop(L, 1);
		}
		lua_getfield(L, tbl, "command");
		if (lua_type(L, -1) == LUA_TSTRING) {
			size_t len = 0;
			const char *v = lua_tolstring(L, -1, &len);
			out->command = std::string(v, std::min<size_t>(len, 4096));
		}
		else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
			out->command = "";
		lua_pop(L, 1);
	}, "the bell policy");
	return got;
}

bool script_rules(struct wb_server *, const ScriptWindow &w, wb::AppRule *out) {
	if (g == nullptr || g->policies.count("rules") == 0)
		return false;
	bool got = false;
	call_policy(g->policies["rules"], [&w](lua_State *L) {
		push_window_table(L, w);
		return 1;
	}, 1, [&](lua_State *L) {
		if (!lua_istable(L, -1))
			return;
		int t = lua_gettop(L);
		got = true;
		lua_getfield(L, t, "desktop");
		if (lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "all") == 0)
			out->desktop = wb::AppRule::kAllDesktops;
		else if (lua_isnumber(L, -1) && lua_tointeger(L, -1) >= 1)
			out->desktop = static_cast<int>(std::min<lua_Integer>(lua_tointeger(L, -1), 1000)) - 1;
		lua_pop(L, 1);
		out->maximized = bool_field(L, t, "maximized");
		out->fullscreen = bool_field(L, t, "fullscreen");
		out->iconic = bool_field(L, t, "minimized");
		out->focus = bool_field(L, t, "focus");
		out->decor = bool_field(L, t, "decor");
		out->x = int_field(L, t, "x");
		out->y = int_field(L, t, "y");
		if (auto v = int_field(L, t, "width"); v && *v >= 1)
			out->width = v;
		if (auto v = int_field(L, t, "height"); v && *v >= 1)
			out->height = v;
	}, "the rules policy");
	return got;
}

std::optional<wb::Rect> script_place(struct wb_server *, const ScriptWindow &w,
		const wb::Rect &area, const wb::Rect &proposed) {
	if (g == nullptr || g->policies.count("place") == 0)
		return std::nullopt;
	std::optional<wb::Rect> placed;
	call_policy(g->policies["place"], [&](lua_State *L) {
		push_window_table(L, w);
		push_rect(L, area);
		push_rect(L, proposed);
		return 3;
	}, 1, [&](lua_State *L) {
		if (!lua_istable(L, -1))
			return;
		int t = lua_gettop(L);
		wb::Rect r = proposed;
		r.x = int_field(L, t, "x").value_or(r.x);
		r.y = int_field(L, t, "y").value_or(r.y);
		r.width = std::max(1, int_field(L, t, "width").value_or(r.width));
		r.height = std::max(1, int_field(L, t, "height").value_or(r.height));
		placed = r;
	}, "the place policy");
	return placed;
}

namespace {

/* One layout call per output, for the current desktop: the windows on it
 * that float (not maximized, fullscreen, tiled to an edge, minimized, or a
 * dialog), and its usable area. The script returns {[w.id] = {x=, y=,
 * width=, height=}}; windows it leaves out stay where they are. */
void run_layout(struct wb_server *server) {
	if (g == nullptr || g->policies.count("layout") == 0)
		return;
	struct wb_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		std::vector<struct wb_toplevel *> windows;
		struct wb_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {  /* top first */
			if (!t->mapped || t->minimized || t->fullscreen || t->tiled ||
					(t->max_horz && t->max_vert) || view_parent(t) != nullptr ||
					!toplevel_on_current_desktop(t) ||
					t->foreign_toplevel_handle == nullptr ||
					get_active_output(t) != o->wlr_output)
				continue;
			windows.push_back(t);
		}
		std::reverse(windows.begin(), windows.end());  /* oldest on top of the stack last */
		struct wlr_box ob;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &ob);
		wb::Rect area{ob.x + o->usable_area.x, ob.y + o->usable_area.y,
				o->usable_area.width, o->usable_area.height};
		std::map<std::string, wb::Rect> placed;
		bool ok = call_policy(g->policies["layout"], [&](lua_State *L) {
			lua_newtable(L);
			int i = 1;
			for (struct wb_toplevel *w : windows) {
				push_window(L, w);
				lua_rawseti(L, -2, i++);
			}
			push_rect(L, area);
			lua_pushstring(L, o->wlr_output->name);
			lua_pushinteger(L, server->current_desktop + 1);
			return 4;
		}, 1, [&](lua_State *L) {
			if (!lua_istable(L, -1))
				return;
			int t = lua_gettop(L);
			lua_pushnil(L);
			while (lua_next(L, t) != 0) {
				if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
					int e = lua_gettop(L);
					auto x = int_field(L, e, "x"), y = int_field(L, e, "y");
					auto w = int_field(L, e, "width"), h = int_field(L, e, "height");
					if (x && y && w && h && *w >= 1 && *h >= 1)
						placed[lua_tostring(L, -2)] = wb::Rect{*x, *y, *w, *h};
				}
				lua_pop(L, 1);
			}
		}, "the layout policy");
		if (!ok)
			continue;
		for (struct wb_toplevel *w : windows) {
			auto it = placed.find(w->foreign_toplevel_handle->identifier);
			if (it == placed.end())
				continue;
			const wb::Rect &r = it->second;
			w->geometry.x = r.x;
			w->geometry.y = r.y;
			if (r.width != w->geometry.width || r.height != w->geometry.height) {
				w->geometry.width = std::min(r.width, 32767);
				w->geometry.height = std::min(r.height, 32767);
				view_set_size(w, w->geometry.width, w->geometry.height);
			}
			position_toplevel(w);
		}
		wlr_log(WLR_DEBUG, "wb-layout %s: %zu of %zu window(s)", o->wlr_output->name,
				placed.size(), windows.size());
	}
}

}  // namespace

void script_request_layout(struct wb_server *server) {
	if (g == nullptr || g->policies.count("layout") == 0 || g->layout_idle != nullptr)
		return;
	g->layout_idle = wl_event_loop_add_idle(server->wl_event_loop, [](void *data) {
		g->layout_idle = nullptr;
		run_layout(static_cast<struct wb_server *>(data));
		wlr_log(WLR_INFO, "%s", "wb-layout done");
	}, server);
}

void scripts_reap(struct wb_server *) {
	/* A stopped program's leader: reaped once its group has ended, else
	 * held for the SIGKILL. */
	for (auto it = stopping.begin(); it != stopping.end();) {
		if (leader_exited(it->pid) && !wb::group_has_others(it->pid, it->pid)) {
			reap_leader(it->pid);
			it = stopping.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = unreaped.begin(); it != unreaped.end();) {
		if (std::any_of(stopping.begin(), stopping.end(),
				[&](const Stopping &st) { return st.pid == *it; })) {
			++it;
			continue;
		}
		int status = 0;
		if (waitpid(*it, &status, WNOHANG) != *it) {
			++it;
			continue;
		}
		pid_t pid = *it;
		it = unreaped.erase(it);
		if (g == nullptr)
			continue;
		for (auto &s : g->spawns) {
			if (s->pid != pid)
				continue;
			s->exited = true;
			s->status = status;
			if (s->eof) {
				spawn_finish(s.get());
			} else {
				/* Its output is still open (a program it started holds
				 * it): what came so far, after a moment. */
				Spawn *raw = s.get();
				raw->grace = wl_event_loop_add_timer(g->server->wl_event_loop,
						[](void *d) {
					auto *sp = static_cast<Spawn *>(d);
					wl_event_source_remove(sp->grace);
					sp->grace = nullptr;
					spawn_finish(sp);
					return 0;
				}, raw);
				if (raw->grace != nullptr)
					wl_event_source_timer_update(raw->grace, 500);
				else
					spawn_finish(raw);
			}
			break;
		}
	}
}

bool script_key_binding(struct wb_server *, uint32_t sym, uint32_t modifiers) {
	if (g == nullptr || g->bindings.empty())
		return false;
	std::vector<wb::KeyBinding> tree;
	for (const Binding &b : g->bindings) {
		wb::KeyBinding k;
		k.sym = b.sym;
		k.modifiers = b.modifiers;
		k.actions.push_back(wb::Action{});  /* a leaf: fires */
		tree.push_back(std::move(k));
	}
	wb::ChainStep step = wb::chain_step(tree, sym, modifiers);
	if (step.outcome != wb::ChainOutcome::Fire || step.node == nullptr)
		return false;
	int ref = g->bindings[static_cast<size_t>(step.node - tree.data())].ref;
	call(ref, [](lua_State *L) {
		push_window(L, first_toplevel(g->server));
		return 1;
	}, "a key binding");
	return true;
}

std::unique_ptr<wb::MenuFile> script_build_menu(struct wb_server *, const std::string &id) {
	if (g == nullptr)
		return nullptr;
	auto p = g->menu_providers.find(id);
	if (p == g->menu_providers.end())
		return nullptr;
	auto file = std::make_unique<wb::MenuFile>();
	wb::Menu menu;
	menu.id = id;
	int serial = 1;
	/* As a policy: it decides what's in the menu, it doesn't act. */
	bool ok = call_policy(p->second, nullptr, 1, [&](lua_State *L) {
		int t = lua_gettop(L);
		if (!lua_istable(L, t))
			return;
		menu.label = string_field(L, t, "label");
		build_items(L, t, menu, *file, serial, 0);
	}, "a menu provider");
	if (!ok)
		note_only(menu, "(script menu failed)");
	else if (menu.items.empty())
		note_only(menu, "(empty)");
	file->menus.push_back(std::move(menu));
	return file;
}

std::string script_menu_label(struct wb_server *, const std::string &id) {
	if (g == nullptr)
		return "";
	auto it = g->menu_labels.find(id);
	return it != g->menu_labels.end() ? it->second : "";
}

void script_menu_call(struct wb_server *server, int callback) {
	if (g == nullptr || session_locked(server))
		return;
	auto it = g->menu_callbacks.find(callback);
	if (it == g->menu_callbacks.end())
		return;
	call(it->second, [server](lua_State *L) {
		push_window(L, first_toplevel(server));
		return 1;
	}, "a menu entry");
}

void script_menu_reset(struct wb_server *) {
	if (g == nullptr || g->L == nullptr)
		return;
	for (const auto &[id, ref] : g->menu_callbacks)
		luaL_unref(g->L, LUA_REGISTRYINDEX, ref);
	g->menu_callbacks.clear();
}

#else  /* no Lua */

void init_scripts(struct wb_server *server) {
	if (server->config != nullptr && !server->config->scripts.empty())
		wlr_log(WLR_ERROR, "%s", "<scripts> set, but Waybox was built without Lua");
}
void finish_scripts(struct wb_server *) {}
void scripts_reap(struct wb_server *) {}
bool script_rules(struct wb_server *, const ScriptWindow &, wb::AppRule *) { return false; }
bool script_bell(struct wb_server *, struct wb_toplevel *, ScriptBell *) { return false; }
void script_bell_event(struct wb_server *, struct wb_toplevel *, int) {}
std::optional<wb::Rect> script_place(struct wb_server *, const ScriptWindow &, const wb::Rect &,
		const wb::Rect &) { return std::nullopt; }
void script_request_layout(struct wb_server *) {}
void scripts_reload(struct wb_server *) {}
void script_window_event(struct wb_toplevel *, const char *) {}
void script_desktop_event(struct wb_server *) {}
void script_output_event(struct wb_server *, const char *, const char *) {}
bool script_key_binding(struct wb_server *, uint32_t, uint32_t) { return false; }
std::unique_ptr<wb::MenuFile> script_build_menu(struct wb_server *, const std::string &) {
	return nullptr;
}
std::string script_menu_label(struct wb_server *, const std::string &) { return ""; }
void script_menu_call(struct wb_server *, int) {}
void script_menu_reset(struct wb_server *) {}

#endif
