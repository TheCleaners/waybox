/*
 * The scripting socket (#154): newline-delimited JSON on a Unix socket in
 * $XDG_RUNTIME_DIR, for scripts in any language (and waybox-msg). Off
 * unless <waybox><ipc enabled="yes"/>. See ipc.h for the requests.
 */
#include "waybox/stall.hpp"
#include "waybox/wlroots.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <vector>

#include "startup_supervisor.h"
#include "config.h"
#include "ipc.h"
#include "color_pick.h"
#include "bell.h"
#include "script.h"
#include "waybox/seat.h"
#include "waybox/session_lock.h"
#include "version.h"
#include "waybox/action.hpp"
#include "waybox/desktops.hpp"
#include "waybox/json.hpp"
#include "waybox/output.h"
#include "waybox/desktop_popup.hpp"
#include "waybox/xdg_shell.h"
#include "waybox/menu.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/style.hpp"

using wb::json::Array;
using wb::json::Object;
using wb::json::Value;

namespace {

constexpr size_t kMaxRequest = 64 * 1024;       /* a line, newline not counted */
constexpr size_t kMaxPending = 4 * 1024 * 1024;  /* queued for a client (it goes past it) */
constexpr size_t kMaxQueued = 16 * 1024 * 1024;  /* queued for all of them together */
constexpr size_t kMaxClients = 64;
constexpr int kMaxPerTurn = 32;  /* requests answered before others get a turn */
constexpr size_t kMaxIdBytes = 128;

/* An answer handle() leaves for later (a color pick replies itself). */
const char *const kLater = "__later";

struct Client {
	struct wb_server *server = nullptr;
	struct wb_ipc_state *st = nullptr;  /* outlives server->ipc while closing */
	int fd = -1;
	struct wl_event_source *source = nullptr;
	std::string in, out;
	size_t out_off = 0;  /* sent of `out` so far */
	std::set<std::string> events;  /* subscribed to */
	bool closing = false;
	bool picking = false;  /* waiting for a color pick's answer */
	bool overflow = false;  /* more queued than allowed: it goes */
	bool hangup = false;    /* it went (or sent too much): it goes once answered */
	/* Its request is running now; and the socket stopped meanwhile (a
	 * reconfigure it asked for turned it off): answered, then dropped. */
	bool answering = false, ending = false;
	/* The "id" of the request being answered, and of the pick waiting for
	 * its answer (echoed in its reply). */
	std::optional<Value> id, pick_id;
};

}  // namespace

struct wb_ipc_state {
	struct wl_event_loop *loop = nullptr;
	int fd = -1;
	std::string path;
	struct wl_event_source *source = nullptr;
	std::vector<std::unique_ptr<Client>> clients;
	/* Clients are only freed from here (an idle callback), never while one
	 * of their requests runs: an action can reconfigure (and stop the
	 * socket) or make an event that drops the client asking. */
	struct wl_event_source *reap = nullptr;
	/* Clients with requests left over (more than kMaxPerTurn at once):
	 * answered on the next turn of the event loop. */
	struct wl_event_source *resume = nullptr;
	size_t queued = 0;  /* bytes queued for every client together */
	bool dead = false;  /* stopped: freed with the last client */
};

namespace {

void reap_now(struct wb_ipc_state *st) {
	if (st->reap != nullptr) {
		wl_event_source_remove(st->reap);
		st->reap = nullptr;
	}
	st->clients.erase(std::remove_if(st->clients.begin(), st->clients.end(),
			[](const std::unique_ptr<Client> &c) { return c->closing; }),
			st->clients.end());
	if (st->dead && st->clients.empty()) {
		if (st->resume != nullptr)
			wl_event_source_remove(st->resume);
		delete st;
	}
}

/* Close a client now; free it later (see wb_ipc_state::reap). */
void drop(struct wb_ipc_state *st, Client *c) {
	if (c->closing)
		return;
	c->closing = true;
	/* Its pick ends now (unanswered): it mustn't outlive the client. */
	if (c->picking) {
		c->picking = false;
		color_pick_cancel(c->server, "the client went");
	}
	if (c->source != nullptr) {
		wl_event_source_remove(c->source);
		c->source = nullptr;
	}
	close(c->fd);
	c->fd = -1;
	st->queued -= c->out.size() - c->out_off;  /* never sent now */
	c->out.clear();
	c->out_off = 0;
	/* Freed from an idle callback; if one can't be had, at the next
	 * connection (on_listen), or when the socket stops. */
	if (st->reap == nullptr)
		st->reap = wl_event_loop_add_idle(st->loop, [](void *data) {
			auto *st = static_cast<struct wb_ipc_state *>(data);
			st->reap = nullptr;  /* idle sources go after running */
			reap_now(st);
		}, st);
}

/* Every reply and event goes through here: refused (and the client
 * marked to go) past kMaxPending queued for it or kMaxQueued for all,
 * before it's added, never after. */
bool enqueue(Client *c, const std::string &line) {
	struct wb_ipc_state *st = c->st;
	if (c->closing || c->overflow)
		return false;
	size_t pending = c->out.size() - c->out_off;
	if (pending + line.size() > kMaxPending || st->queued + line.size() > kMaxQueued) {
		c->overflow = true;
		wlr_log(WLR_ERROR, "wb-ipc: a client isn't reading its replies; it goes");
		return false;
	}
	c->out += line;
	st->queued += line.size();
	return true;
}

/* Send what's queued; keep the rest for when the socket can take it.
 * False when the client must go (its socket failed, or it overflowed). */
bool flush(Client *c) {
	if (c->closing)
		return true;
	struct wb_ipc_state *st = c->st;
	while (c->out_off < c->out.size()) {
		ssize_t n = send(c->fd, c->out.data() + c->out_off, c->out.size() - c->out_off,
				MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		}
		c->out_off += static_cast<size_t>(n);
		st->queued -= static_cast<size_t>(n);
	}
	/* Sent: let the buffer go (or, half of it sent, the front of it). */
	if (c->out_off == c->out.size()) {
		c->out.clear();
		c->out_off = 0;
	} else if (c->out_off > 64 * 1024 && c->out_off * 2 > c->out.size()) {
		c->out.erase(0, c->out_off);
		c->out_off = 0;
	}
	wl_event_source_fd_update(c->source, WL_EVENT_READABLE |
			(c->out_off < c->out.size() ? WL_EVENT_WRITABLE : 0));
	return !c->overflow;
}

/* A reply, with the request's "id" in it if it had one. */
void reply(Client *c, Value v, const std::optional<Value> &id = std::nullopt) {
	if (id != std::nullopt)
		if (auto *o = std::get_if<Object>(&v.v))
			(*o)["id"] = *id;
	enqueue(c, wb::json::dump(v) + "\n");
}

Value error(const std::string &message) {
	return Object{{"ok", false}, {"error", message}};
}

bool focused(struct wb_toplevel *t) {
	struct wlr_surface *s = view_surface(t);
	return s != nullptr && t->server->seat->seat->keyboard_state.focused_surface == s;
}

Value window_json(struct wb_toplevel *t) {
	Object w;
	w["id"] = t->foreign_toplevel_handle != nullptr
			? Value(std::string(t->foreign_toplevel_handle->identifier)) : Value();
	w["app_id"] = view_app_id(t) ? view_app_id(t) : "";
	w["title"] = view_title(t) ? view_title(t) : "";
	w["tag"] = t->tag;
	w["desktop"] = t->desktop + 1;
	w["omnipresent"] = t->omnipresent;
	w["urgent"] = t->urgent;
	w["layer"] = t->layer > 0 ? "above" : t->layer < 0 ? "below" : "normal";
	w["focused"] = focused(t);
	w["minimized"] = t->minimized;
	/* Drawn now: on a desktop its monitor shows, not minimized. */
	w["shown"] = t->mapped && t->scene_tree != nullptr && t->scene_tree->node.enabled &&
			!t->minimized;
	struct wlr_output *out = get_active_output(t);
	w["output"] = out != nullptr ? out->name : "";
	w["maximized"] = t->max_horz && t->max_vert;
	w["fullscreen"] = t->fullscreen;
	w["shaded"] = t->shaded;
	w["x"] = t->geometry.x;
	w["y"] = t->geometry.y;
	w["width"] = t->geometry.width;
	w["height"] = t->geometry.height;
	w["xwayland"] = t->kind == wb_view_kind::Xwayland;
	return w;
}

Value desktop_json(struct wb_server *server, int index) {
	const wb::DesktopConfig &cfg = server->config ? server->config->desktops
			: wb::DesktopConfig{};
	return Object{{"number", index + 1}, {"name", wb::desktop_name(cfg, index)},
			{"current", index == server->current_desktop}};
}

/* Each monitor and the desktop it shows (with shared desktops, the same
 * one on all; per monitor, each its own; #367). */
Value monitors_json(struct wb_server *server) {
	Array list;
	struct wb_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output->enabled)
			list.push_back(Object{{"output", std::string(o->wlr_output->name)},
					{"desktop", output_desktop(server, o->wlr_output) + 1}});
	return list;
}

/* The window a request names: "focused" (the default), an id, "app_id:X"
 * or "title:X" (the first, most recently used, that matches). */
struct wb_toplevel *find_window(struct wb_server *server, const std::string &sel) {
	struct wb_toplevel *t;
	if (sel.empty() || sel == "focused") {
		wl_list_for_each(t, &server->toplevels, link)
			if (t->mapped && focused(t))
				return t;
		return nullptr;
	}
	wl_list_for_each(t, &server->focus_order, focus_link) {
		if (!t->mapped)
			continue;
		const char *app = view_app_id(t), *title = view_title(t);
		if (sel.rfind("app_id:", 0) == 0 && app && sel.substr(7) == app)
			return t;
		if (sel.rfind("title:", 0) == 0 && title && sel.substr(6) == title)
			return t;
		if (t->foreign_toplevel_handle != nullptr &&
				sel == t->foreign_toplevel_handle->identifier)
			return t;
	}
	return nullptr;
}

Value handle(Client *c, const Value &req) {
	struct wb_server *server = c->server;
	std::string cmd = req.str("request");
	/* While the screen is locked: nothing that runs a command, restarts a
	 * program or acts on the session (as key and mouse bindings can't).
	 * Queries and subscriptions stay, for bars and other session services. */
	static const std::set<std::string> kNotWhileLocked = {
			"action", "bell", "restart_program", "preview_menu", "pick_color", "pick_window"};
	if (session_locked(server) && kNotWhileLocked.count(cmd))
		return error("the screen is locked");
	/* Only with WAYBOX_TEST_GPU_RESET=1: as if the GPU was reset. */
	if (cmd == "test_renderer_lost") {
		if (!wb_test_renderer_lost(server))
			return error("no test GPU reset");
		return Object{{"ok", true}};
	}
	/* What blocked the event loop (see stall.hpp). */
	/* Read rc.xml again and apply it (Reconfigure), answering once it's
	 * done: applied, or kept as it was, with why (wbconf says which). */
	if (cmd == "reconfigure") {
		std::string problem;
		if (!wb_reconfigure(server, &problem))
			return Object{{"ok", false}, {"applied", false},
					{"error", problem.empty() ? std::string("rc.xml can't be read") : problem}};
		return Object{{"ok", true}, {"applied", true}};
	}
	if (cmd == "get_stalls") {
		Array scopes;
		for (const auto &[name, st] : wb::Stalls::get().stats())
			scopes.push_back(Object{{"name", name}, {"count", static_cast<double>(st.count)},
					{"slow", static_cast<double>(st.slow)}, {"total_ms", st.total_ms},
					{"max_ms", st.max_ms},
					{"mean_ms", st.count ? st.total_ms / static_cast<double>(st.count) : 0.0}});
		return Object{{"ok", true}, {"threshold_ms", wb::Stalls::get().threshold_ms},
				{"scopes", scopes}};
	}
	/* Only with WAYBOX_TEST_TOUCH=1 (the tests' fake touchscreen). */
	if (cmd == "test_touch") {
		auto num = [&req](const char *k) {
			const Value *v = req.get(k);
			return v != nullptr && v->number() != nullptr ? *v->number() : 0.0;
		};
		if (!seat_test_touch(server, req.str("event"),
				static_cast<int32_t>(std::clamp(num("id"), -1e6, 1e6)),
				num("x"), num("y")))
			return error("no test touchscreen");
		return Object{{"ok", true}};
	}
	/* Restart a program Waybox started (wbconf's Reload=restart): "name",
	 * its startup name, or "binary", its command's program. */
	if (cmd == "restart_program") {
		std::string name = req.str("name").substr(0, 256);
		std::string binary = req.str("binary").substr(0, 256);
		if (name.empty() && binary.empty())
			return error("restart_program needs a name or binary");
		std::string why = startup_restart(server, name, binary);
		if (!why.empty())
			return error(why);
		return Object{{"ok", true}};
	}
	/* Ring the bell, as the focused window would (wbconf's Test): with
	 * "flash", "color", "duration", "count", "width", "command", those
	 * settings over rc.xml's, as a preview (no policy, no event). */
	if (cmd == "bell") {
		ScriptBell preview;
		bool any = false;
		for (const char *k : {"flash", "color", "duration", "count", "width"})
			if (const Value *v = req.get(k)) {
				any = true;
				if (v->string() != nullptr)
					preview.over[k] = v->string()->substr(0, 64);
				else if (v->number() != nullptr)
					preview.over[k] = std::to_string(static_cast<long>(
							std::clamp(*v->number(), -1e9, 1e9)));
			}
		if (const Value *v = req.get("command"); v != nullptr && v->string() != nullptr) {
			any = true;
			preview.command = v->string()->substr(0, 4096);
		}
		std::string shown = bell_ring(server, first_toplevel(server), any ? &preview : nullptr);
		return Object{{"ok", true}, {"shown", shown}};
	}
	/* An on-screen display shown for a moment (wbconf's Test): "message"
	 * (else "On-screen display"), with "position", "opacity", "cornerRadius",
	 * "bg", "text", "border" over rc.xml's <osd>, as a preview. */
	if (cmd == "osd") {
		wb::OsdConfig base = server->config != nullptr ? server->config->osd : wb::OsdConfig{};
		auto str = [&req](const char *k) -> std::string {
			const Value *v = req.get(k);
			if (v == nullptr)
				return "";
			if (v->string() != nullptr)
				return v->string()->substr(0, 64);
			if (v->number() != nullptr)
				return std::to_string(static_cast<long>(std::clamp(*v->number(), -1e9, 1e9)));
			return "";
		};
		std::vector<std::string> problems;
		wb::OsdConfig over = wb::parse_osd_config(str("position"), str("opacity"),
				str("cornerRadius"), str("bg"), str("text"), str("border"), &problems);
		if (!problems.empty())
			return error("not an on-screen display setting: " + problems.front());
		if (req.get("position") != nullptr)
			base.position = over.position;
		if (over.opacity) base.opacity = over.opacity;
		if (over.corner_radius) base.corner_radius = over.corner_radius;
		if (over.bg) base.bg = over.bg;
		if (over.text) base.text = over.text;
		if (over.border) base.border = over.border;
		wb::Theme theme = server->config != nullptr ? server->config->theme
				: wb::default_theme();
		std::string message = "On-screen display";
		if (const Value *m = req.get("message"); m != nullptr && m->string() != nullptr &&
				!m->string()->empty())
			message = m->string()->substr(0, 200);
		if (server->desktop_popup != nullptr)
			server->desktop_popup->hide();  /* not two at once */
		server->osd_preview = std::make_unique<wb::DesktopPopup>(server,
				wb::osd_style(theme, base));
		server->osd_preview->show(message, 2000);
		return Object{{"ok", true}};
	}
	/* A color from the screen (wbconf's eyedropper): the pointer becomes a
	 * crosshair, and the answer comes with the click ({"ok": true,
	 * "color": "#rrggbb"}), or an error (cancelled, timed out). */
	if (cmd == "pick_color") {
		struct wb_ipc_state *st = c->st;
		bool started = color_pick_start(server, [c, st](bool ok, const wb::Color &col,
				const std::string &why) {
			if (c->closing || !c->picking)
				return;
			c->picking = false;
			if (ok) {
				char hex[8];
				snprintf(hex, sizeof hex, "#%02x%02x%02x", col.r, col.g, col.b);
				reply(c, Object{{"ok", true}, {"color", std::string(hex)}}, c->pick_id);
			} else {
				reply(c, error(why), c->pick_id);
			}
			if (!flush(c))
				drop(st, c);
		});
		if (!started)
			return error("a color pick is already on");
		c->picking = true;
		return Value(Object{{kLater, true}});
	}
	/* A window, clicked (wbconf's Pick a Window): its details, as
	 * get_windows gives them ({"ok": true, "window": {...}}), or an error
	 * (no window there, cancelled, timed out). */
	if (cmd == "pick_window") {
		struct wb_ipc_state *st = c->st;
		bool started = window_pick_start(server, [c, st](struct wb_toplevel *t,
				const std::string &why) {
			if (c->closing || !c->picking)
				return;
			c->picking = false;
			if (t != nullptr)
				reply(c, Object{{"ok", true}, {"window", window_json(t)}}, c->pick_id);
			else
				reply(c, error(why), c->pick_id);
			if (!flush(c))
				drop(st, c);
		});
		if (!started)
			return error("a pick is already on");
		c->picking = true;
		return Value(Object{{kLater, true}});
	}
	/* A menu, from menu.xml text not saved yet (wbconf's menu editor), shown
	 * at the pointer as Waybox draws it; an entry chosen closes it, its
	 * actions dropped (wb_run_menu_actions: a preview's Exit mustn't end
	 * the session). */
	if (cmd == "preview_menu") {
		if (session_locked(server))
			return error("the screen is locked");
		std::string id = req.str("id");
		if (id.empty())
			id = "root-menu";
		auto file = std::make_unique<wb::MenuFile>(wb::parse_menu_xml(req.str("menu")));
		if (file->find(id) == nullptr)
			return error("no menu " + id);
		/* The open one first: it may be showing the last preview. */
		server->menu.reset();
		auto widget = std::make_unique<wb::MenuWidget>(server, *file,
				wb::menu_style_from_theme(server->config->theme), server->config->menu_behavior);
		if (!widget->open(id, static_cast<int>(server->cursor->cursor->x),
				static_cast<int>(server->cursor->cursor->y)))
			return error("couldn't open " + id);
		server->preview_menu = std::move(file);
		server->menu = std::move(widget);
		server->menu_target = nullptr;
		return Object{{"ok", true}};
	}
	/* Each monitor's name, large, on it (wbconf's Identify). */
	if (cmd == "identify") {
		const Value *ms = req.get("ms");
		int duration = ms != nullptr && ms->number() != nullptr
				? static_cast<int>(std::clamp(*ms->number(), 100.0, 30000.0)) : 3000;
		int n = wb_identify_outputs(server, req.str("output"), duration);
		if (n == 0 && !req.str("output").empty())
			return error("no such monitor");
		return Object{{"ok", true}, {"shown", n}};
	}
	if (cmd == "get_version")
		return Object{{"ok", true}, {"name", "waybox"}, {"version", WB_BUILD_VERSION},
				{"release", PACKAGE_VERSION}};
	if (cmd == "get_windows") {
		Array list;
		struct wb_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link)
			if (t->mapped)
				list.push_back(window_json(t));
		return Object{{"ok", true}, {"windows", list}};
	}
	if (cmd == "get_desktops") {
		Array list;
		for (int i = 0; i < desktop_count(server); ++i)
			list.push_back(desktop_json(server, i));
		return Object{{"ok", true}, {"desktops", list}, {"monitors", monitors_json(server)}};
	}
	if (cmd == "get_outputs") {
		Array list;
		struct wb_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			struct wlr_output *wo = o->wlr_output;
			list.push_back(Object{{"name", wo->name},
					{"description", wo->description ? wo->description : ""},
					{"enabled", wo->enabled || o->powered_off},
					{"powered", wo->enabled}, {"x", o->geometry.x},
					{"y", o->geometry.y}, {"width", o->geometry.width},
					{"height", o->geometry.height}, {"scale", static_cast<double>(wo->scale)}});
		}
		return Object{{"ok", true}, {"outputs", list}};
	}
	if (cmd == "action") {
		/* As key and mouse bindings: nothing happens behind the lock
		 * screen (no menu, switcher or window changes). */
		if (session_locked(server))
			return error("the screen is locked");
		std::string name = req.str("action");
		auto action = wb::make_action(name, req.str("command"));
		if (!action)
			return error("unknown action: " + name);
		if (req.get("to"))
			action->to = req.str("to");
		if (req.get("direction"))
			action->direction = req.str("direction");
		if (req.get("where"))
			action->direction = req.str("where");
		if (const Value *w = req.get("wrap"); w && w->boolean())
			action->wrap = *w->boolean();
		if (const Value *f = req.get("follow"); f && f->boolean())
			action->follow = *f->boolean();
		for (const char *k : {"x", "y", "width", "height", "monitor", "left", "right", "top",
				"bottom", "prompt"})  /* prompt: Exit's (a logout script's "no") */
			if (const Value *v = req.get(k)) {
				if (v->boolean() != nullptr)
					action->args[k] = *v->boolean() ? "yes" : "no";
				else if (v->string() != nullptr)
					action->args[k] = v->string()->substr(0, 64);
				else if (v->number() != nullptr)
					action->args[k] = std::to_string(static_cast<long>(
							std::clamp(*v->number(), -1e6, 1e6)));
			}
		struct wb_toplevel *target = nullptr;
		if (req.get("window")) {
			target = find_window(server, req.str("window"));
			if (target == nullptr)
				return error("no such window: " + req.str("window"));
		}
		/* As a key binding would, on that window (or the focused one). */
		server->action_window = target;
		wb::run_action(*action, server);
		server->action_window = nullptr;
		return Object{{"ok", true}};
	}
	if (cmd == "subscribe") {
		static const std::set<std::string> known = {"window", "desktop", "output", "config",
				"bell"};
		const Value *ev = req.get("events");
		if (ev == nullptr || ev->array() == nullptr)
			return error("subscribe needs \"events\": [\"window\", \"desktop\", \"output\", "
					"\"config\", \"bell\"]");
		/* All checked first: one unknown, and none is subscribed to. */
		for (const Value &e : *ev->array())
			if (e.string() == nullptr || known.count(*e.string()) == 0)
				return error("unknown event: " + wb::json::dump(e));
		for (const Value &e : *ev->array())
			c->events.insert(*e.string());
		return Object{{"ok", true}};
	}
	return error("unknown request: " + cmd);
}

/* A request's "id" (a string or a number), echoed in its reply; nothing
 * for one without (or an odd one). */
std::optional<Value> request_id(const Value &req) {
	const Value *id = req.get("id");
	if (id == nullptr)
		return std::nullopt;
	if (id->number() != nullptr || (id->string() != nullptr && id->string()->size() <= kMaxIdBytes))
		return *id;
	return std::nullopt;
}

void schedule_resume(struct wb_ipc_state *st);

/* Answer up to kMaxPerTurn of the client's complete requests (the rest
 * on the next turn); true if some are left. A line longer than
 * kMaxRequest is refused before it's parsed, and the client goes. */
bool answer_lines(Client *c) {
	size_t nl;
	int turn = 0;
	/* (Answered after a hang-up too: a request sent just before closing.) */
	while (!c->closing && (nl = c->in.find('\n')) != std::string::npos) {
		if (turn++ == kMaxPerTurn)
			return true;
		if (nl > kMaxRequest) {
			reply(c, error("a request longer than 64 KiB"));
			c->in.clear();
			c->hangup = true;
			return false;
		}
		std::string line = c->in.substr(0, nl);
		c->in.erase(0, nl + 1);
		if (line.find_first_not_of(" \t\r") == std::string::npos)
			continue;
		auto req = wb::json::parse(line);
		c->id = req && req->object() ? request_id(*req) : std::nullopt;
		c->answering = true;
		Value answer = req && req->object() ? handle(c, *req)
				: error("not a JSON object");
		c->answering = false;
		/* The request may have dropped this client (or stopped the
		 * socket): it's still allocated, just closed. An answer for
		 * later (a pick) is sent when it comes, with this one's id. */
		bool later = answer.object() != nullptr && answer.object()->count(kLater);
		if (later)
			c->pick_id = c->id;
		else if (!c->closing)
			reply(c, std::move(answer), c->id);
		c->id.reset();
		if (c->ending) {
			/* The socket is gone: this answer sent now (a moment at most,
			 * it's small), then the client. */
			for (int tries = 0; tries < 50 && !c->closing; ++tries) {
				if (!flush(c) || c->out_off >= c->out.size())
					break;  /* sent (or the client's gone) */
				struct pollfd pfd = {c->fd, POLLOUT, 0};
				poll(&pfd, 1, 20);
			}
			drop(c->st, c);
			return false;
		}
	}
	return false;
}

/* Answered, then what's queued sent; the client goes if it must. */
void after_answering(struct wb_ipc_state *st, Client *c, bool more) {
	if (c->closing)
		return;
	if (!c->hangup && c->in.size() > kMaxRequest && c->in.find('\n') == std::string::npos) {
		/* A line already too long, no end in sight: refused, not read on. */
		reply(c, error("a request longer than 64 KiB"));
		c->in.clear();
		c->hangup = true;
	}
	if (!flush(c) || (c->hangup && !more))
		drop(st, c);
	else if (more)
		schedule_resume(st);
}

void schedule_resume(struct wb_ipc_state *st) {
	if (st->resume != nullptr)
		return;
	st->resume = wl_event_loop_add_idle(st->loop, [](void *data) {
		auto *st = static_cast<struct wb_ipc_state *>(data);
		st->resume = nullptr;
		std::vector<Client *> waiting;
		for (auto &c : st->clients)
			if (!c->closing && c->in.find('\n') != std::string::npos)
				waiting.push_back(c.get());
		for (Client *c : waiting) {
			bool more = answer_lines(c);
			after_answering(st, c, more);
		}
	}, st);
}

int on_client(int fd, uint32_t mask, void *data) {
	auto *c = static_cast<Client *>(data);
	struct wb_ipc_state *st = c->st;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		c->hangup = true;
	/* What a client sent before hanging up (write and close at once) is
	 * still handled. */
	if (mask & WL_EVENT_READABLE) {
		/* Everything there is now (bounded): after a hang-up there's no
		 * second chance to read the rest. */
		char buf[4096];
		while (c->in.size() <= kMaxRequest + 1) {
			ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (n > 0) {
				c->in.append(buf, static_cast<size_t>(n));
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
				c->hangup = true;
			break;
		}
	}
	bool more = answer_lines(c);
	after_answering(st, c, more);
	return 0;
}

int on_listen(int fd, uint32_t, void *data) {
	auto *server = static_cast<struct wb_server *>(data);
	int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (cfd < 0)
		return 0;
	/* Only the user who runs Waybox (the socket's mode says so too). */
#ifdef SO_PEERCRED
	struct ucred cred = {};
	socklen_t len = sizeof(cred);
	bool ours = getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
			cred.uid == getuid();
#else
	uid_t uid = 0;
	gid_t gid = 0;
	bool ours = getpeereid(cfd, &uid, &gid) == 0 && uid == getuid();
#endif
	if (!ours) {
		close(cfd);
		return 0;
	}
	struct wb_ipc_state *st = server->ipc;
	/* Clients closed but not freed (no idle callback could be had): here,
	 * outside any client's callback, they can be. */
	if (st->reap == nullptr && std::any_of(st->clients.begin(), st->clients.end(),
			[](const std::unique_ptr<Client> &c) { return c->closing; }))
		reap_now(st);
	/* Enough at once: this one is refused (accepted and closed, so the
	 * listener isn't readable for it forever). */
	if (st->clients.size() >= kMaxClients) {
		close(cfd);
		wlr_log(WLR_ERROR, "wb-ipc: %zu clients already; one more refused", kMaxClients);
		return 0;
	}
	auto c = std::make_unique<Client>();
	c->server = server;
	c->st = st;
	c->fd = cfd;
	c->source = wl_event_loop_add_fd(server->wl_event_loop, cfd, WL_EVENT_READABLE,
			on_client, c.get());
	if (c->source == nullptr) {
		close(cfd);
		return 0;
	}
	st->clients.push_back(std::move(c));
	return 0;
}

/* `now`: free everything at once (Waybox exiting, nothing dispatching);
 * else (a Reconfigure, maybe from a client's own request) the clients
 * and the state go from the idle callback. */
void stop(struct wb_server *server, bool now) {
	struct wb_ipc_state *st = server->ipc;
	if (st == nullptr)
		return;
	for (auto &c : st->clients) {
		if (c->answering && !now)
			c->ending = true;  /* its answer first (answer_lines drops it) */
		else
			drop(st, c.get());
	}
	if (st->source != nullptr)
		wl_event_source_remove(st->source);
	st->source = nullptr;
	if (st->fd >= 0)
		close(st->fd);
	st->fd = -1;
	if (!st->path.empty())
		unlink(st->path.c_str());
	unsetenv("WAYBOX_SOCK");
	server->ipc = nullptr;
	st->dead = true;
	wlr_log(WLR_INFO, "%s", "wb-ipc stopped");
	if (now || st->clients.empty())
		reap_now(st);
}

bool start(struct wb_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (runtime == nullptr || display == nullptr)
		return false;
	auto *st = new wb_ipc_state;
	st->loop = server->wl_event_loop;
	st->path = std::string(runtime) + "/waybox-" + display + ".sock";
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (st->path.size() >= sizeof(addr.sun_path)) {
		delete st;
		return false;
	}
	memcpy(addr.sun_path, st->path.c_str(), st->path.size() + 1);
	st->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	unlink(st->path.c_str());  /* left by a Waybox that crashed */
	mode_t old = umask(0177);
	bool ok = st->fd >= 0 &&
			bind(st->fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 &&
			listen(st->fd, 16) == 0;
	umask(old);
	if (!ok) {
		wlr_log(WLR_ERROR, "Couldn't open the scripting socket %s: %s", st->path.c_str(),
				strerror(errno));
		if (st->fd >= 0)
			close(st->fd);
		delete st;
		return false;
	}
	chmod(st->path.c_str(), 0600);
	st->source = wl_event_loop_add_fd(server->wl_event_loop, st->fd, WL_EVENT_READABLE,
			on_listen, server);
	if (st->source == nullptr) {
		/* Nothing would answer: no socket said to be there. */
		wlr_log(WLR_ERROR, "Couldn't listen on the scripting socket %s", st->path.c_str());
		close(st->fd);
		unlink(st->path.c_str());
		delete st;
		return false;
	}
	server->ipc = st;
	setenv("WAYBOX_SOCK", st->path.c_str(), true);
	wlr_log(WLR_INFO, "wb-ipc listening %s", st->path.c_str());
	return true;
}

void send_event(struct wb_server *server, const std::string &type, Object body) {
	struct wb_ipc_state *st = server->ipc;
	if (st == nullptr)
		return;
	body["event"] = type;
	std::string line = wb::json::dump(Value(body)) + "\n";
	std::vector<Client *> gone;
	for (auto &c : st->clients) {
		if (c->closing || c->events.count(type) == 0)
			continue;
		enqueue(c.get(), line);
		if (!flush(c.get()))
			gone.push_back(c.get());
	}
	for (Client *c : gone)
		drop(st, c);
}

}  // namespace

void ipc_reconfigured(struct wb_server *server) {
	bool want = server->config != nullptr && server->config->ipc_enabled;
	if (want && server->ipc == nullptr)
		start(server);
	else if (!want && server->ipc != nullptr)
		stop(server, false);
	send_event(server, "config", Object{{"change", "reload"}});
}

void ipc_finish(struct wb_server *server) {
	stop(server, true);
}

void ipc_window_event(struct wb_toplevel *toplevel, const char *change) {
	script_window_event(toplevel, change);
	if (toplevel->server->ipc == nullptr)
		return;
	send_event(toplevel->server, "window",
			Object{{"change", change}, {"window", window_json(toplevel)}});
}

void ipc_desktop_event(struct wb_server *server) {
	script_desktop_event(server);
	if (server->ipc == nullptr)
		return;
	send_event(server, "desktop", Object{{"change", "switch"},
			{"desktop", desktop_json(server, server->current_desktop)},
			{"monitors", monitors_json(server)}});
}

void ipc_bell_event(struct wb_server *server, struct wb_toplevel *t, const std::string &shown,
		int skipped) {
	if (server->ipc == nullptr)
		return;
	/* skipped: rings since the last event that came too soon to count. */
	Object body{{"shown", shown}, {"skipped", skipped}};
	if (t != nullptr && t->mapped)
		body["window"] = window_json(t);
	send_event(server, "bell", body);
}

void ipc_output_event(struct wb_server *server, struct wlr_output *output, const char *change) {
	script_output_event(server, output->name, change);
	if (server->ipc == nullptr)
		return;
	send_event(server, "output", Object{{"change", change}, {"name", output->name}});
}
