/*
 * The compositor's own agent tools (agent_tools_provider.hpp).
 */
#include "waybox/agent_tools_provider.hpp"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <functional>
#include <map>
#include <string>

#include "../agent/mcp_agent.hpp"
#include "ipc.h"
#include "version.h"
#include "waybox/output.h"
#include "waybox/server.h"
#include "waybox/xdg_shell.h"

using wb::json::Array;
using wb::json::Object;
using wb::json::Value;

namespace {

/* What the hub may send at once (its MaxFrame). */
constexpr size_t kMaxFrame = 4 * 1024 * 1024;

struct Tool {
	const char *description;  /* (the hub gives agents the manifest's) */
	std::function<Object(struct wb_server *)> run;
};

Object windows(struct wb_server *server, bool titles) {
	Array list;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->mapped)
			continue;
		Object w = *ipc_window_json(t).object();
		if (titles) {
			list.push_back(Object{{"id", w["id"]}, {"title", w["title"]}});
			continue;
		}
		w.erase("title");
		uid_t uid = 0;
		pid_t pid = view_pid(t, &uid);
		w["pid"] = pid > 0 ? Value(static_cast<double>(pid)) : Value();
		/* X11 windows aren't pinged: unknown. */
		w["answers"] = t->kind == wb_view_kind::Xwayland ? Value() : Value(!t->not_responding);
		list.push_back(std::move(w));
	}
	return Object{{"windows", list}};
}

const std::map<std::string, Tool> &tools() {
	static const std::map<std::string, Tool> t = {
		{"list_windows", {"The open windows, without their titles.",
				[](struct wb_server *s) { return windows(s, false); }}},
		{"window_titles", {"Each open window's id and title.",
				[](struct wb_server *s) { return windows(s, true); }}},
		{"list_desktops", {"The desktops and which each monitor shows.",
				[](struct wb_server *s) {
					Array list;
					for (int i = 0; i < desktop_count(s); ++i)
						list.push_back(ipc_desktop_json(s, i));
					return Object{{"desktops", list}, {"monitors", ipc_monitors_json(s)}};
				}}},
		{"list_outputs", {"The monitors.",
				[](struct wb_server *s) {
					Array list;
					struct wb_output *o;
					wl_list_for_each(o, &s->outputs, link) {
						struct wlr_output *wo = o->wlr_output;
						list.push_back(Object{
							{"name", wo->name},
							{"description", wo->description ? wo->description : ""},
							{"enabled", wo->enabled || o->powered_off},
							{"powered", wo->enabled},
							{"x", o->geometry.x}, {"y", o->geometry.y},
							{"width", o->geometry.width}, {"height", o->geometry.height},
							{"scale", static_cast<double>(wo->scale)},
							{"mode", Object{{"width", wo->width}, {"height", wo->height},
									{"refresh_mhz", wo->refresh}}},
						});
					}
					return Object{{"outputs", list}};
				}}},
		{"get_version", {"The compositor's name and version.",
				[](struct wb_server *) {
					return Object{{"name", "waybox"}, {"version", WB_BUILD_VERSION},
							{"release", PACKAGE_VERSION}};
				}}},
	};
	return t;
}

/* The tools, on the main thread: each answered at once. */
class Tools : public wb::agent::ToolSource {
public:
	explicit Tools(struct wb_server *server) : server_(server) {}
	Array list_tools() override {
		Array out;
		for (const auto &[name, tool] : tools())
			out.push_back(Object{
				{"name", name},
				{"description", tool.description},
				{"inputSchema", Object{{"type", "object"}, {"properties", Object{}},
						{"additionalProperties", false}}},
			});
		return out;
	}
	void call(wb::agent::McpServer &server, const Value &id, const std::string &name,
			const Object &args) override {
		auto it = tools().find(name);
		if (it == tools().end()) {
			server.fail(id, -32602, "unknown tool: " + name);
			return;
		}
		if (!args.empty()) {
			server.fail(id, -32602, name + " takes no arguments");
			return;
		}
		Object result = it->second.run(server_);
		server.complete(id, Object{
			{"content", Array{Object{{"type", "text"}, {"text", wb::json::dump(result)}}}},
			{"structuredContent", std::move(result)},
		});
	}

private:
	struct wb_server *server_;
};

}  // namespace

struct AgentToolsProvider {
	struct wb_server *server = nullptr;
	int fd = -1;
	struct wl_event_source *source = nullptr;
	Tools tools{nullptr};
	wb::agent::McpServer mcp{"waybox", WB_BUILD_VERSION, 8, &tools};
	std::string in, out;
};

namespace {

void stop_reading(AgentToolsProvider *p) {
	if (p->source != nullptr) {
		wl_event_source_remove(p->source);
		p->source = nullptr;
	}
}

/* What's queued, written as far as the socket takes it; the rest waits for
 * it to be writable. */
void flush(AgentToolsProvider *p) {
	while (!p->out.empty()) {
		ssize_t n = write(p->fd, p->out.data(), p->out.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			wlr_log_errno(WLR_ERROR, "agent tools: the hub's tools socket");
			p->out.clear();
			stop_reading(p);
			return;
		}
		p->out.erase(0, static_cast<size_t>(n));
	}
	if (p->source != nullptr)
		wl_event_source_fd_update(p->source,
				WL_EVENT_READABLE | (p->out.empty() ? 0 : WL_EVENT_WRITABLE));
}

int on_fd(int, uint32_t mask, void *data) {
	auto *p = static_cast<AgentToolsProvider *>(data);
	if (mask & WL_EVENT_WRITABLE)
		flush(p);
	if (mask & WL_EVENT_READABLE) {
		char buf[65536];
		for (;;) {
			ssize_t n = read(p->fd, buf, sizeof(buf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n <= 0) {
				stop_reading(p);  /* the hub went (its supervision sees it) */
				return 0;
			}
			p->in.append(buf, static_cast<size_t>(n));
			size_t start = 0;
			for (size_t nl; (nl = p->in.find('\n', start)) != std::string::npos; start = nl + 1) {
				std::string_view line(p->in.data() + start, nl - start);
				if (!line.empty())
					for (std::string &o : p->mcp.receive(line))
						p->out += o + '\n';
			}
			p->in.erase(0, start);
			if (p->in.size() > kMaxFrame) {
				wlr_log(WLR_ERROR, "agent tools: a message from the hub past MaxFrame");
				stop_reading(p);
				return 0;
			}
		}
		flush(p);
	}
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		stop_reading(p);
	return 0;
}

}  // namespace

AgentToolsProvider *agent_tools_provider_start(struct wb_server *server, int fd) {
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return nullptr;
	}
	auto *p = new AgentToolsProvider;
	p->server = server;
	p->tools = Tools(server);
	p->fd = fd;
	p->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd,
			WL_EVENT_READABLE, on_fd, p);
	return p;
}

void agent_tools_provider_stop(AgentToolsProvider *p) {
	if (p == nullptr)
		return;
	stop_reading(p);
	if (p->fd >= 0)
		close(p->fd);
	delete p;
}
