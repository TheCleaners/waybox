/*
 * The hub's agent socket (agents.hpp).
 */
#include "agents.hpp"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/ucred.h>
#include <sys/user.h>
#endif

#include <errno.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>

#include "mcp_agent.hpp"
#include "tunables.hpp"
#include "waybox/platform.hpp"

namespace wb::agent {

namespace {

constexpr gsize kReadChunk = 64 * 1024;

void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	gchar *text = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_printerr("waybox-agent-hub: %s\n", text);
	g_free(text);
}

struct Conn {
	unsigned id = 0;
	GSocketConnection *sc = nullptr;
	GCancellable *cancel = g_cancellable_new();
	std::string buf;
	bool handshaken = false;
	bool done_reading = false;
	guint deadline = 0;
	std::deque<std::shared_ptr<std::string>> outq;
	size_t queued = 0;     /* bytes in outq and being written */
	bool writing = false;
	bool paused = false;   /* past MaxQueued: requests wait unread */
	bool listens_closed = false;
	std::unique_ptr<AgentEndpoint> endpoint;
	std::string who;  /* for the log: "agent 3" */
	~Conn() {
		if (deadline != 0)
			g_source_remove(deadline);
		g_cancellable_cancel(cancel);
		if (sc != nullptr) {
			g_io_stream_close(G_IO_STREAM(sc), nullptr, nullptr);
			g_object_unref(sc);
		}
		g_object_unref(cancel);
	}
};

struct Agents {
	Tunables t;
	GSocketService *service = nullptr;
	std::string path, hub_version;
	unsigned next_id = 1;
	std::map<unsigned, std::unique_ptr<Conn>> conns;
};

Agents *g_agents = nullptr;

Conn *find(unsigned id) {
	if (g_agents == nullptr)
		return nullptr;
	auto it = g_agents->conns.find(id);
	return it != g_agents->conns.end() ? it->second.get() : nullptr;
}

void close_conn(unsigned id, const char *why) {
	Conn *c = find(id);
	if (c == nullptr)
		return;
	say("%s: left (%s)", c->who.c_str(), why);
	g_agents->conns.erase(id);
}

void read_more(Conn *c);
void write_next(Conn *c);
bool take_lines(Conn *c);

/* What the agent is told: after the handshake, or MCP. */
void send(Conn *c, std::string line) {
	line += '\n';
	c->queued += line.size();
	c->outq.push_back(std::make_shared<std::string>(std::move(line)));
	if (!c->writing)
		write_next(c);
}

struct WriteOp {
	unsigned id;
	std::shared_ptr<std::string> data;  /* kept until the write ends */
};

/* The agent is done sending and every request is answered: open listen
 * streams get their closing results (a graceful end), then, once all is
 * written, the hub closes. */
bool finishing(Conn *c) {
	if (!c->done_reading || c->paused)
		return false;
	if (!c->listens_closed) {
		c->listens_closed = true;
		if (c->endpoint)
			for (std::string &line : c->endpoint->close_listens()) {
				line += '\n';
				c->queued += line.size();
				c->outq.push_back(std::make_shared<std::string>(std::move(line)));
			}
	}
	return true;
}

void write_next(Conn *c) {
	if (c->outq.empty() && finishing(c) && c->outq.empty()) {
		c->writing = false;
		close_conn(c->id, "done");
		return;
	}
	if (c->outq.empty()) {
		c->writing = false;
		return;
	}
	c->writing = true;
	auto *op = new WriteOp{c->id, c->outq.front()};
	c->outq.pop_front();
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c->sc));
	g_output_stream_write_all_async(out, op->data->data(), op->data->size(), G_PRIORITY_DEFAULT,
			c->cancel, [](GObject *src, GAsyncResult *res, gpointer data) {
				std::unique_ptr<WriteOp> op(static_cast<WriteOp *>(data));
				GError *err = nullptr;
				bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, nullptr, &err);
				bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
				g_clear_error(&err);
				if (cancelled)
					return;
				Conn *c = find(op->id);
				if (c == nullptr)
					return;
				if (!ok) {
					close_conn(op->id, "its socket closed");
					return;
				}
				c->queued -= op->data->size();
				/* Read again once the agent has caught up. */
				if (c->paused && c->queued < g_agents->t.max_queued / 2) {
					c->paused = false;
					if (!take_lines(c))
						return;
					if (!c->paused && !c->done_reading)
						read_more(c);
				}
				write_next(c);
			}, op);
}

/* The peer's user and process, from the socket itself (GLib has no
 * credentials on FreeBSD): false if they can't be had. */
bool peer_of(int fd, uid_t &uid, pid_t &pid) {
#if defined(__linux__)
	struct ucred cr {};
	socklen_t len = sizeof(cr);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
		return false;
	uid = cr.uid;
	pid = cr.pid;
	return true;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	struct xucred cr {};
	socklen_t len = sizeof(cr);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cr, &len) != 0 || cr.cr_version != XUCRED_VERSION)
		return false;
	uid = cr.cr_uid;
	pid = cr.cr_pid;  /* (FreeBSD 13 and later) */
	return true;
#else
	WB_PLATFORM_FALLBACK("a connecting agent's process (its user is still checked)");
	pid = 0;
	gid_t gid;
	return getpeereid(fd, &uid, &gid) == 0;
#endif
}

/* The executable of the process that started `pid` (the agent that
 * started a shim): "" if it can't be known. */
std::string parent_executable(pid_t pid) {
#if defined(__linux__)
	gchar *stat = nullptr;
	std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
	if (!g_file_get_contents(stat_path.c_str(), &stat, nullptr, nullptr))
		return {};
	const char *close = strrchr(stat, ')');
	int ppid = 0;
	bool ok = close != nullptr && sscanf(close + 1, " %*c %d", &ppid) == 1 && ppid > 1;
	g_free(stat);
	if (!ok)
		return {};
	std::string exe_path = "/proc/" + std::to_string(ppid) + "/exe";
	gchar *exe = g_file_read_link(exe_path.c_str(), nullptr);
	std::string out = exe != nullptr ? exe : "";
	g_free(exe);
	return out;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
	struct kinfo_proc kp {};
	size_t len = sizeof(kp);
	if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len < sizeof(kp) || kp.ki_ppid <= 1)
		return {};
	int path_mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, static_cast<int>(kp.ki_ppid)};
	char path[PATH_MAX];
	size_t plen = sizeof(path);
	if (sysctl(path_mib, 4, path, &plen, nullptr, 0) != 0 || plen == 0)
		return {};
	return std::string(path, strnlen(path, plen));
#else
	WB_PLATFORM_FALLBACK("the agent that started a shim (for the log)");
	(void) pid;
	return {};
#endif
}

/* Who's on the other end: only this user; its process, and the agent
 * that started it (shown and audited, never trusted). */
bool identify(Conn *c) {
	int fd = g_socket_get_fd(g_socket_connection_get_socket(c->sc));
	uid_t uid = 0;
	pid_t pid = 0;
	if (!peer_of(fd, uid, pid)) {
		say("%s: no credentials (%s): refused", c->who.c_str(), g_strerror(errno));
		return false;
	}
	if (uid != getuid()) {
		say("%s: another user's: refused", c->who.c_str());
		return false;
	}
	std::string desc = "pid ?";
	if (pid > 0) {
		desc = "pid " + std::to_string(pid);
		std::string agent = parent_executable(pid);
		if (!agent.empty())
			desc += ", agent " + agent;
	}
	say("%s: connected (%s)", c->who.c_str(), desc.c_str());
	return true;
}

void on_line(Conn *c, std::string_view line) {
	if (!c->handshaken) {
		if (c->deadline != 0) {
			g_source_remove(c->deadline);
			c->deadline = 0;
		}
		Handshake hs = read_handshake(line);
		const std::string profile = "safe";
		send(c, handshake_answer(hs, profile, "waybox-agent-hub " + g_agents->hub_version));
		if (!hs.ok) {
			say("%s: handshake refused: %s", c->who.c_str(), hs.error.c_str());
			c->done_reading = true;  /* closed once the answer is out */
			return;
		}
		c->handshaken = true;
		c->endpoint = std::make_unique<AgentEndpoint>("waybox-agent-hub", g_agents->hub_version,
				g_agents->t.max_listens);
		if (!hs.label.empty())
			c->who += " (" + display_name(hs.label) + ")";
		say("%s: profile %s%s", c->who.c_str(), profile.c_str(),
				hs.token ? " (a token no binding has)" : "");
		return;
	}
	auto era = c->endpoint->era();
	for (std::string &out : c->endpoint->receive(line))
		send(c, std::move(out));
	if (era == AgentEndpoint::Era::Unknown && c->endpoint->era() != era)
		say("%s: speaks MCP %s%s", c->who.c_str(),
				c->endpoint->era() == AgentEndpoint::Era::Stateless ? "2026-07-28" : "with initialize",
				c->endpoint->claimed_name().empty() ? ""
						: (" as \"" + display_name(c->endpoint->claimed_name()) + "\"").c_str());
	for (const std::string &n : c->endpoint->take_notes())
		say("%s: %s", c->who.c_str(), n.c_str());
}

void on_read(GObject *src, GAsyncResult *res, gpointer data) {
	unsigned id = GPOINTER_TO_UINT(data);
	GError *err = nullptr;
	GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
	if (bytes == nullptr) {
		bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_clear_error(&err);
		if (!cancelled)
			close_conn(id, "a read failed");
		return;
	}
	Conn *c = find(id);
	if (c == nullptr) {
		g_bytes_unref(bytes);
		return;
	}
	gsize n = 0;
	const char *p = static_cast<const char *>(g_bytes_get_data(bytes, &n));
	if (n == 0) {
		g_bytes_unref(bytes);
		/* Done sending: what's still to answer goes out (open listen
		 * streams closed, as a graceful end), then the hub closes. */
		c->done_reading = true;
		if (!take_lines(c))
			return;
		if (!c->writing)
			write_next(c);  /* (finishes, if nothing waits) */
		return;
	}
	c->buf.append(p, n);
	g_bytes_unref(bytes);
	if (!take_lines(c))
		return;
	if (!c->paused && !c->done_reading)
		read_more(c);
}

/* The whole lines read so far, taken in turn, until the agent's answers
 * pass MaxQueued (the rest wait, unread). False if the connection closed. */
bool take_lines(Conn *c) {
	unsigned id = c->id;
	size_t start = 0;
	bool ok = true;
	for (size_t nl; (nl = c->buf.find('\n', start)) != std::string::npos;) {
		if (c->queued >= g_agents->t.max_queued) {
			c->paused = true;
			break;
		}
		std::string_view line(c->buf.data() + start, nl - start);
		start = nl + 1;
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.size() > g_agents->t.max_frame) {
			close_conn(id, "a message longer than MaxFrame");
			return false;
		}
		if (!line.empty())
			on_line(c, line);
		if (find(id) == nullptr)
			return false;
		if (!c->handshaken && c->done_reading) {
			ok = true;  /* refused: nothing more is read */
			c->buf.clear();
			return true;
		}
	}
	c->buf.erase(0, start);
	if (c->buf.size() > g_agents->t.max_frame && c->buf.find('\n') == std::string::npos) {
		close_conn(id, "a message longer than MaxFrame");
		return false;
	}
	return ok;
}

void read_more(Conn *c) {
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(c->sc));
	g_input_stream_read_bytes_async(in, kReadChunk, G_PRIORITY_DEFAULT, c->cancel, on_read,
			GUINT_TO_POINTER(c->id));
}

gboolean on_incoming(GSocketService *, GSocketConnection *sc, GObject *, gpointer) {
	if (g_agents == nullptr)
		return FALSE;
	if (g_agents->conns.size() >= g_agents->t.max_agents) {
		say("an agent refused: %zu connected already (MaxAgents)", g_agents->conns.size());
		return TRUE;  /* (closed: not kept) */
	}
	auto c = std::make_unique<Conn>();
	c->id = g_agents->next_id++;
	c->sc = G_SOCKET_CONNECTION(g_object_ref(sc));
	c->who = "agent " + std::to_string(c->id);
	if (!identify(c.get()))
		return TRUE;  /* (closed with c) */
	c->deadline = g_timeout_add(static_cast<guint>(g_agents->t.handshake_wait.count()),
			[](gpointer data) -> gboolean {
		unsigned id = GPOINTER_TO_UINT(data);
		if (Conn *c = find(id)) {
			c->deadline = 0;
			close_conn(id, "no handshake within HandshakeWait");
		}
		return G_SOURCE_REMOVE;
	}, GUINT_TO_POINTER(c->id));
	Conn *raw = c.get();
	g_agents->conns.emplace(c->id, std::move(c));
	read_more(raw);
	return TRUE;
}

}  // namespace

bool agents_listen(const std::string &path, const std::string &hub_version,
		const Tunables &t) {
	auto *a = new Agents;
	a->t = t;
	a->path = path;
	a->hub_version = hub_version;
	unlink(path.c_str());  /* a stale one, in the hub's own folder */
	a->service = g_socket_service_new();
	GSocketAddress *addr = g_unix_socket_address_new(path.c_str());
	GError *err = nullptr;
	bool ok = g_socket_listener_add_address(G_SOCKET_LISTENER(a->service), addr,
			G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, &err);
	g_object_unref(addr);
	if (!ok) {
		say("%s: %s", path.c_str(), err->message);
		g_clear_error(&err);
		g_object_unref(a->service);
		delete a;
		return false;
	}
	g_signal_connect(a->service, "incoming", G_CALLBACK(on_incoming), nullptr);
	g_socket_service_start(a->service);
	g_agents = a;
	say("listening for agents at %s", path.c_str());
	return true;
}

void agents_stop() {
	Agents *a = g_agents;
	if (a == nullptr)
		return;
	g_socket_service_stop(a->service);
	g_socket_listener_close(G_SOCKET_LISTENER(a->service));
	g_object_unref(a->service);
	a->conns.clear();
	g_agents = nullptr;
	unlink(a->path.c_str());
	delete a;
}

}  // namespace wb::agent
