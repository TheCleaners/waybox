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
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

using wb::json::Array;
using wb::json::Object;
using wb::json::Value;

namespace {

constexpr size_t kMaxRequest = 64 * 1024;
constexpr size_t kMaxPending = 4 * 1024 * 1024;  /* a client not reading goes */

/* An answer handle() leaves for later (a color pick replies itself). */
const char *const kLater = "__later";

struct Client {
	struct wb_server *server = nullptr;
	struct wb_ipc_state *st = nullptr;  /* outlives server->ipc while closing */
	int fd = -1;
	struct wl_event_source *source = nullptr;
	std::string in, out;
	std::set<std::string> events;  /* subscribed to */
	bool closing = false;
	bool picking = false;  /* waiting for a color pick's answer */
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
	if (st->dead && st->clients.empty())
		delete st;
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
	if (st->reap == nullptr)
		st->reap = wl_event_loop_add_idle(st->loop, [](void *data) {
			auto *st = static_cast<struct wb_ipc_state *>(data);
			st->reap = nullptr;  /* idle sources go after running */
			reap_now(st);
		}, st);
}

/* Send what's queued; keep the rest for when the socket can take it. */
bool flush(Client *c) {
	if (c->closing)
		return true;
	while (!c->out.empty()) {
		ssize_t n = send(c->fd, c->out.data(), c->out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		}
		c->out.erase(0, static_cast<size_t>(n));
	}
	wl_event_source_fd_update(c->source, WL_EVENT_READABLE |
			(c->out.empty() ? 0 : WL_EVENT_WRITABLE));
	return c->out.size() <= kMaxPending;
}

void reply(Client *c, const Value &v) {
	c->out += wb::json::dump(v);
	c->out += '\n';
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
	/* Only with WAYBOX_TEST_GPU_RESET=1: as if the GPU was reset. */
	if (cmd == "test_renderer_lost") {
		if (!wb_test_renderer_lost(server))
			return error("no test GPU reset");
		return Object{{"ok", true}};
	}
	/* What blocked the event loop (see stall.hpp). */
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
				reply(c, Object{{"ok", true}, {"color", std::string(hex)}});
			} else {
				reply(c, error(why));
			}
			if (!flush(c))
				drop(st, c);
		});
		if (!started)
			return error("a color pick is already on");
		c->picking = true;
		return Value(Object{{kLater, true}});
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
		return Object{{"ok", true}, {"desktops", list}};
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
		if (const Value *w = req.get("wrap"); w && w->boolean())
			action->wrap = *w->boolean();
		if (const Value *f = req.get("follow"); f && f->boolean())
			action->follow = *f->boolean();
		for (const char *k : {"x", "y", "width", "height", "monitor", "left", "right", "top",
				"bottom"})
			if (const Value *v = req.get(k)) {
				if (v->string() != nullptr)
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
		for (const Value &e : *ev->array()) {
			if (e.string() == nullptr || known.count(*e.string()) == 0)
				return error("unknown event: " + wb::json::dump(e));
			c->events.insert(*e.string());
		}
		return Object{{"ok", true}};
	}
	return error("unknown request: " + cmd);
}

int on_client(int fd, uint32_t mask, void *data) {
	auto *c = static_cast<Client *>(data);
	struct wb_ipc_state *st = c->st;
	bool hangup = mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR);
	/* What a client sent before hanging up (write and close at once) is
	 * still handled. */
	if (mask & WL_EVENT_READABLE) {
		/* Everything there is now (bounded): after a hang-up there's no
		 * second chance to read the rest. */
		char buf[4096];
		while (c->in.size() <= kMaxRequest) {
			ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (n > 0) {
				c->in.append(buf, static_cast<size_t>(n));
				continue;
			}
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
				hangup = true;
			break;
		}
		size_t nl;
		while (!c->closing && (nl = c->in.find('\n')) != std::string::npos) {
			std::string line = c->in.substr(0, nl);
			c->in.erase(0, nl + 1);
			if (line.find_first_not_of(" \t\r") == std::string::npos)
				continue;
			auto req = wb::json::parse(line);
			Value answer = req && req->object() ? handle(c, *req)
					: error("not a JSON object");
			/* The request may have dropped this client (or stopped the
			 * socket): it's still allocated, just closed. An answer for
			 * later (a color pick) is sent when it comes. */
			bool later = answer.object() != nullptr && answer.object()->count(kLater);
			if (!c->closing && !later)
				reply(c, answer);
		}
		if (c->in.size() > kMaxRequest)
			hangup = true;
	}
	if (c->closing)
		return 0;
	if (!flush(c) || hangup)
		drop(st, c);
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
	auto c = std::make_unique<Client>();
	c->server = server;
	c->st = server->ipc;
	c->fd = cfd;
	c->source = wl_event_loop_add_fd(server->wl_event_loop, cfd, WL_EVENT_READABLE,
			on_client, c.get());
	server->ipc->clients.push_back(std::move(c));
	return 0;
}

/* `now`: free everything at once (Waybox exiting, nothing dispatching);
 * else (a Reconfigure, maybe from a client's own request) the clients
 * and the state go from the idle callback. */
void stop(struct wb_server *server, bool now) {
	struct wb_ipc_state *st = server->ipc;
	if (st == nullptr)
		return;
	for (auto &c : st->clients)
		drop(st, c.get());
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
		c->out += line;
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
			{"desktop", desktop_json(server, server->current_desktop)}});
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
