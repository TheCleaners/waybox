/*
 * The scripting socket (#154): newline-delimited JSON on a Unix socket in
 * $XDG_RUNTIME_DIR, for scripts in any language (and waybox-msg). Off
 * unless <waybox><ipc enabled="yes"/>. See ipc.h for the requests.
 */
#include "waybox/wlroots.hpp"

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

#include "config.h"
#include "ipc.h"
#include "version.h"
#include "waybox/action.hpp"
#include "waybox/desktops.hpp"
#include "waybox/json.hpp"
#include "waybox/output.h"
#include "waybox/xdg_shell.h"

using wb::json::Array;
using wb::json::Object;
using wb::json::Value;

namespace {

constexpr size_t kMaxRequest = 64 * 1024;
constexpr size_t kMaxPending = 4 * 1024 * 1024;  /* a client not reading goes */

struct Client {
	struct wb_server *server = nullptr;
	int fd = -1;
	struct wl_event_source *source = nullptr;
	std::string in, out;
	std::set<std::string> events;  /* subscribed to */
	bool closing = false;
};

}  // namespace

struct wb_ipc_state {
	int fd = -1;
	std::string path;
	struct wl_event_source *source = nullptr;
	std::vector<std::unique_ptr<Client>> clients;
};

namespace {

void drop(struct wb_ipc_state *st, Client *c) {
	if (c->source != nullptr)
		wl_event_source_remove(c->source);
	close(c->fd);
	for (auto it = st->clients.begin(); it != st->clients.end(); ++it)
		if (it->get() == c) {
			st->clients.erase(it);
			return;
		}
}

/* Send what's queued; keep the rest for when the socket can take it. */
bool flush(Client *c) {
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
	w["desktop"] = t->desktop + 1;
	w["omnipresent"] = t->omnipresent;
	w["focused"] = focused(t);
	w["minimized"] = t->minimized;
	w["maximized"] = t->max_horz && t->max_vert;
	w["fullscreen"] = t->fullscreen;
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
		static const std::set<std::string> known = {"window", "desktop", "output", "config"};
		const Value *ev = req.get("events");
		if (ev == nullptr || ev->array() == nullptr)
			return error("subscribe needs \"events\": [\"window\", \"desktop\", \"output\", \"config\"]");
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
	struct wb_ipc_state *st = c->server->ipc;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		drop(st, c);
		return 0;
	}
	if (mask & WL_EVENT_READABLE) {
		char buf[4096];
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			drop(st, c);
			return 0;
		}
		if (n > 0)
			c->in.append(buf, static_cast<size_t>(n));
		size_t nl;
		while ((nl = c->in.find('\n')) != std::string::npos) {
			std::string line = c->in.substr(0, nl);
			c->in.erase(0, nl + 1);
			if (line.find_first_not_of(" \t\r") == std::string::npos)
				continue;
			auto req = wb::json::parse(line);
			reply(c, req && req->object() ? handle(c, *req)
					: error("not a JSON object"));
		}
		if (c->in.size() > kMaxRequest) {
			drop(st, c);
			return 0;
		}
	}
	if (!flush(c))
		drop(st, c);
	return 0;
}

int on_listen(int fd, uint32_t, void *data) {
	auto *server = static_cast<struct wb_server *>(data);
	int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (cfd < 0)
		return 0;
	/* Only the user who runs Waybox (the socket's mode says so too). */
	struct ucred cred = {};
	socklen_t len = sizeof(cred);
	if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
		close(cfd);
		return 0;
	}
	auto c = std::make_unique<Client>();
	c->server = server;
	c->fd = cfd;
	c->source = wl_event_loop_add_fd(server->wl_event_loop, cfd, WL_EVENT_READABLE,
			on_client, c.get());
	server->ipc->clients.push_back(std::move(c));
	return 0;
}

void stop(struct wb_server *server) {
	struct wb_ipc_state *st = server->ipc;
	if (st == nullptr)
		return;
	while (!st->clients.empty())
		drop(st, st->clients.back().get());
	if (st->source != nullptr)
		wl_event_source_remove(st->source);
	if (st->fd >= 0)
		close(st->fd);
	if (!st->path.empty())
		unlink(st->path.c_str());
	unsetenv("WAYBOX_SOCK");
	delete st;
	server->ipc = nullptr;
	wlr_log(WLR_INFO, "%s", "wb-ipc stopped");
}

bool start(struct wb_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (runtime == nullptr || display == nullptr)
		return false;
	auto *st = new wb_ipc_state;
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
		if (c->events.count(type) == 0)
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
		stop(server);
	send_event(server, "config", Object{{"change", "reload"}});
}

void ipc_finish(struct wb_server *server) {
	stop(server);
}

void ipc_window_event(struct wb_toplevel *toplevel, const char *change) {
	if (toplevel->server->ipc == nullptr)
		return;
	send_event(toplevel->server, "window",
			Object{{"change", change}, {"window", window_json(toplevel)}});
}

void ipc_desktop_event(struct wb_server *server) {
	if (server->ipc == nullptr)
		return;
	send_event(server, "desktop", Object{{"change", "switch"},
			{"desktop", desktop_json(server, server->current_desktop)}});
}

void ipc_output_event(struct wb_server *server, struct wlr_output *output, const char *change) {
	if (server->ipc == nullptr)
		return;
	send_event(server, "output", Object{{"change", change}, {"name", output->name}});
}
