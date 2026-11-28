/*
 * The compositor's side of org.freedesktop.AgentCompositor1
 * (agent_compositor.hpp).
 */
#include "waybox/agent_compositor.hpp"

#include <gio/gio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <latch>
#include <thread>
#include <vector>

extern char **environ;

#include "agent_compositor_xml.h"
#include "waybox/server.h"
#include "waybox/session_lock.h"
#include "waybox/agent_tools_provider.hpp"
#include "waybox/async.hpp"
#include "waybox/glib_thread.hpp"
#include "waybox/threads.hpp"

namespace {

constexpr const char *kPath = "/org/freedesktop/AgentCompositor1";
constexpr const char *kIface = "org.freedesktop.AgentCompositor1";
constexpr guint32 kVersion = 1;
/* Tunables (doc/agent-tools.md, "Tunables"): HubRestarts, 3 in 1 min; the
 * delay before each restart grows from a second. */
constexpr size_t kHubRestarts = 3;
constexpr std::chrono::seconds kHubRestartWindow{60};
constexpr std::chrono::milliseconds kHubRestartDelay{1000};

/* What the hub may read at any time, from the D-Bus thread: kept by the
 * main thread. */
struct Snapshot {
	std::mutex mutex;
	bool locked = false, away = false, stopped = false;
	GDBusConnection *conn = nullptr;  /* set once connected; ref held */
};

/* The connection, on the shared GLib thread: only ever touched there. */
struct Conn {
	GCancellable *cancel = g_cancellable_new();
	GDBusConnection *conn = nullptr;
	guint registration = 0;
	std::shared_ptr<Snapshot> snap;
	std::weak_ptr<wb::MainQueue> queue;
	/* The hub closed it: the main thread ends that hub (once). */
	std::function<void()> on_closed;
	~Conn() { g_object_unref(cancel); }
};

/* For the session, while agent tools are on: the hub it runs (one at a
 * time, each with a connection of its own) and the state that outlives
 * any one hub (the snapshot: the stopped state, kept through restarts). */
struct AgentCompositor {
	struct wb_server *server = nullptr;
	std::string command;
	pid_t hub = -1;
	std::shared_ptr<Snapshot> snap = std::make_shared<Snapshot>();
	std::shared_ptr<Conn> conn;
	AgentToolsProvider *tools = nullptr;  /* this hub's: the compositor's tools */
	std::deque<std::chrono::steady_clock::time_point> crashes;
	bool given_up = false;
	wb::TaskScope restarts;  /* a restart waiting its delay (dropped with this) */
};

AgentCompositor *g_ac = nullptr;  /* one per compositor */

GVariant *bool_prop(bool v) { return g_variant_new_boolean(v); }

void on_method(GDBusConnection *, const char *, const char *, const char *,
		const char *method, GVariant *, GDBusMethodInvocation *invocation, gpointer data) {
	auto *snap = static_cast<Snapshot *>(data);
	if (g_strcmp0(method, "Hello") == 0) {
		bool locked;
		{
			std::lock_guard lock(snap->mutex);
			locked = snap->locked;
		}
		GVariantBuilder b;
		g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string("waybox"));
		/* What this version supports: not yet capture, input or the
		 * indicator (their steps bring them). */
		g_variant_builder_add(&b, "{sv}", "capture", bool_prop(false));
		g_variant_builder_add(&b, "{sv}", "input", bool_prop(false));
		g_variant_builder_add(&b, "{sv}", "indicator", bool_prop(false));
		g_variant_builder_add(&b, "{sv}", "locked", bool_prop(locked));
		/* The manifest of the compositor's own tools (AGENT_COMPOSITOR_TOOLS_FD). */
		g_variant_builder_add(&b, "{sv}", "provider", g_variant_new_string(kCompositorProviderId));
		g_dbus_method_invocation_return_value(invocation,
				g_variant_new("(ua{sv})", kVersion, &b));
		return;
	}
	g_dbus_method_invocation_return_dbus_error(invocation,
			"org.freedesktop.DBus.Error.NotSupported",
			"not supported by this version of the compositor");
}

GVariant *on_get(GDBusConnection *, const char *, const char *, const char *,
		const char *prop, GError **error, gpointer data) {
	auto *snap = static_cast<Snapshot *>(data);
	std::lock_guard lock(snap->mutex);
	if (g_strcmp0(prop, "version") == 0)
		return g_variant_new_uint32(kVersion);
	if (g_strcmp0(prop, "Locked") == 0)
		return bool_prop(snap->locked);
	if (g_strcmp0(prop, "Away") == 0)
		return bool_prop(snap->away);
	if (g_strcmp0(prop, "AssistantsStopped") == 0)
		return bool_prop(snap->stopped);
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
	return nullptr;
}

void log_on_main(const std::shared_ptr<Conn> &c, std::string text, enum wlr_log_importance level) {
	if (auto q = c->queue.lock())
		q->post([text = std::move(text), level] {
			_wlr_log(level, "agent tools: %s", text.c_str());
		});
}

/* On the GLib thread: the connection made (or failed); its object
 * registered, its closing watched. */
void on_connected(GObject *, GAsyncResult *res, gpointer data) {
	std::unique_ptr<std::shared_ptr<Conn>> holder(static_cast<std::shared_ptr<Conn> *>(data));
	std::shared_ptr<Conn> c = *holder;
	GError *err = nullptr;
	GDBusConnection *conn = g_dbus_connection_new_finish(res, &err);
	if (conn == nullptr) {
		if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			log_on_main(c, std::string("the hub's connection failed: ") + err->message, WLR_ERROR);
		g_clear_error(&err);
		return;
	}
	if (g_cancellable_is_cancelled(c->cancel)) {  /* finished meanwhile */
		g_dbus_connection_close_sync(conn, nullptr, nullptr);
		g_object_unref(conn);
		return;
	}
	c->conn = conn;
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kAgentCompositorXml, &err);
	if (node == nullptr) {
		log_on_main(c, std::string("the interface's description: ") + err->message, WLR_ERROR);
		g_clear_error(&err);
	} else {
		static const GDBusInterfaceVTable vtable = {on_method, on_get, nullptr, {nullptr}};
		c->registration = g_dbus_connection_register_object(conn, kPath, node->interfaces[0],
				&vtable, c->snap.get(), nullptr, &err);
		g_clear_error(&err);
		g_dbus_node_info_unref(node);
	}
	auto *closed_data = new std::weak_ptr<Conn>(c);
	g_signal_connect_data(conn, "closed", G_CALLBACK(+[](GDBusConnection *, gboolean, GError *,
			gpointer d) {
		auto c = static_cast<std::weak_ptr<Conn> *>(d)->lock();
		if (!c || g_cancellable_is_cancelled(c->cancel))
			return;  /* (closed by us) */
		if (auto q = c->queue.lock())
			q->post(c->on_closed);
	}), closed_data, [](gpointer d, GClosure *) {
		delete static_cast<std::weak_ptr<Conn> *>(d);
	}, GConnectFlags(0));
	{
		std::lock_guard lock(c->snap->mutex);
		c->snap->conn = G_DBUS_CONNECTION(g_object_ref(conn));
	}
	/* Only now: a hub calling the moment it's connected finds the object
	 * there (messages wait until this). */
	g_dbus_connection_start_message_processing(conn);
}

/* On the GLib thread: start the connection over our end of the socket. */
void connect_hub(const std::shared_ptr<Conn> &c, int fd) {
	GError *err = nullptr;
	GSocket *sock = g_socket_new_from_fd(fd, &err);
	if (sock == nullptr) {
		close(fd);
		log_on_main(c, std::string("the hub's socket: ") + err->message, WLR_ERROR);
		g_clear_error(&err);
		return;
	}
	GSocketConnection *stream = g_socket_connection_factory_create_connection(sock);
	g_object_unref(sock);
	gchar *guid = g_dbus_generate_guid();
	/* Asynchronous: a hub slow to authenticate never holds the thread
	 * everything GLib in Waybox shares. */
	g_dbus_connection_new(G_IO_STREAM(stream), guid,
			GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
					G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
			nullptr, c->cancel, on_connected,
			new std::shared_ptr<Conn>(c));
	g_free(guid);
	g_object_unref(stream);
}

/* On the GLib thread: the connection ended. */
void disconnect_hub(const std::shared_ptr<Conn> &c) {
	g_cancellable_cancel(c->cancel);
	{
		std::lock_guard lock(c->snap->mutex);
		g_clear_object(&c->snap->conn);
	}
	if (c->conn != nullptr) {
		if (c->registration != 0)
			g_dbus_connection_unregister_object(c->conn, c->registration);
		g_dbus_connection_close_sync(c->conn, nullptr, nullptr);
		g_clear_object(&c->conn);
	}
}

void emit_changed(Snapshot *snap, const char *name, bool value) {
	GDBusConnection *conn;
	{
		std::lock_guard lock(snap->mutex);
		conn = snap->conn != nullptr ? G_DBUS_CONNECTION(g_object_ref(snap->conn)) : nullptr;
	}
	if (conn == nullptr)
		return;
	GVariantBuilder changed;
	g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&changed, "{sv}", name, bool_prop(value));
	/* (Emitting is thread-safe: GDBus sends from its own thread.) */
	g_dbus_connection_emit_signal(conn, nullptr, kPath, "org.freedesktop.DBus.Properties",
			"PropertiesChanged", g_variant_new("(sa{sv}as)", kIface, &changed, nullptr),
			nullptr);
	g_object_unref(conn);
}

}  // namespace

namespace {

/* Agent tools are on: Enabled=true in the spec's settings.ini, the first
 * one found (the user's, then the system's: file by file, as XDG has it). */
bool agent_tools_enabled() {
	std::vector<std::string> dirs;
	dirs.emplace_back(g_get_user_config_dir());
	for (const char *const *d = g_get_system_config_dirs(); *d != nullptr; ++d)
		dirs.emplace_back(*d);
	for (const std::string &dir : dirs) {
		std::string path = dir + "/agents/tools/settings.ini";
		GKeyFile *kf = g_key_file_new();
		GError *err = nullptr;
		bool loaded = g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, &err);
		bool missing = !loaded && g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
		if (!loaded && !missing)
			wlr_log(WLR_ERROR, "agent tools: %s: %s", path.c_str(), err->message);
		g_clear_error(&err);
		bool on = loaded && g_key_file_get_boolean(kf, "Agent Tools", "Enabled", nullptr);
		g_key_file_unref(kf);
		if (!missing)
			return on;
	}
	return false;
}

/* What to run as the hub, when agent tools are on: waybox-agent-hub, or
 * the program WAYBOX_AGENT_HUB names (a stand-in, for tests). */
std::string hub_command() {
	if (!agent_tools_enabled())
		return {};
	const char *hub = getenv("WAYBOX_AGENT_HUB");
	if (hub != nullptr && *hub != '\0')
		return hub;
	gchar *quoted = g_shell_quote(WB_AGENT_HUB);
	std::string cmd = quoted;
	g_free(quoted);
	return cmd;
}

void hub_lost(AgentCompositor *ac);

/* Starts a hub, with a connection of its own. */
void start_hub(AgentCompositor *ac) {
	/* Everything the child needs, made before fork: between fork and exec
	 * only async-signal-safe calls (Waybox has threads). */
	std::vector<std::string> env_store;
	for (char **e = environ; *e != nullptr; ++e)
		if (std::string(*e).rfind("AGENT_COMPOSITOR_FD=", 0) != 0 &&
				std::string(*e).rfind("AGENT_COMPOSITOR_TOOLS_FD=", 0) != 0)
			env_store.emplace_back(*e);
	env_store.emplace_back("AGENT_COMPOSITOR_FD=3");
	env_store.emplace_back("AGENT_COMPOSITOR_TOOLS_FD=4");
	std::vector<char *> envp;
	for (auto &e : env_store)
		envp.push_back(e.data());
	envp.push_back(nullptr);
	const char *argv[] = {"sh", "-c", ac->command.c_str(), nullptr};
	/* Two sockets: the compositor interface (fd 3) and the compositor's
	 * tools (fd 4). */
	int sv[2], tv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		wlr_log_errno(WLR_ERROR, "agent tools: socketpair");
		return;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, tv) != 0) {
		wlr_log_errno(WLR_ERROR, "agent tools: socketpair");
		close(sv[0]);
		close(sv[1]);
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "agent tools: fork");
		close(sv[0]);
		close(sv[1]);
		close(tv[0]);
		close(tv[1]);
		return;
	}
	if (pid == 0) {
		/* The hub: its end as fd 3, named in its environment. */
		setpgid(0, 0);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		/* Out of the way first (either could be 3 or 4), then into
		 * place: dup2's copies aren't close-on-exec. */
		int a = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
		int b = fcntl(tv[1], F_DUPFD_CLOEXEC, 10);
		if (a < 0 || b < 0 || dup2(a, 3) < 0 || dup2(b, 4) < 0)
			_exit(127);
		execve("/bin/sh", const_cast<char *const *>(argv), envp.data());
		_exit(127);
	}
	setpgid(pid, pid);
	close(sv[1]);
	close(tv[1]);
	ac->hub = pid;
	agent_tools_provider_stop(ac->tools);
	ac->tools = agent_tools_provider_start(ac->server, tv[0]);
	auto c = std::make_shared<Conn>();
	c->snap = ac->snap;
	c->queue = ac->server->main_queue;
	/* The hub closing its connection ends it: a hub without one is no
	 * use, and its exit is then a crash like any other. */
	std::weak_ptr<Conn> weak = c;
	c->on_closed = [ac, weak] {
		auto c = weak.lock();
		if (c && g_ac == ac && ac->conn == c && ac->hub > 0) {
			wlr_log(WLR_ERROR, "agent tools: the hub closed its connection");
			kill(-ac->hub, SIGTERM);
		}
	};
	ac->conn = c;
	int fd = sv[0];
	ac->server->glib->post([c, fd] { connect_hub(c, fd); });
	wlr_log(WLR_INFO, "agent tools: started the hub (%d)", static_cast<int>(pid));
}

/* The current connection ended on the GLib thread (waited for, when
 * `wait`: nothing of it outlives the call). */
void end_conn(AgentCompositor *ac, bool wait) {
	agent_tools_provider_stop(ac->tools);
	ac->tools = nullptr;
	if (!ac->conn)
		return;
	std::shared_ptr<Conn> c = std::move(ac->conn);
	if (!wait) {
		ac->server->glib->post([c] { disconnect_hub(c); });
		return;
	}
	std::latch ended(1);
	ac->server->glib->post([c, &ended] {
		disconnect_hub(c);
		ended.count_down();
	});
	ended.wait();
}

/* Waits up to `ms` for the hub to exit; true once it has (reaped). */
bool hub_exited_within(AgentCompositor *ac, int ms) {
	for (int i = 0; ac->hub > 0 && i <= ms / 10; ++i) {
		if (waitpid(ac->hub, nullptr, WNOHANG) == ac->hub) {
			ac->hub = -1;
			return true;
		}
		usleep(10000);
	}
	return ac->hub <= 0;
}

/* Ends the hub on purpose: its socket closes, and it ends by itself; one
 * that doesn't, within a moment, is ended (its process group: what it
 * started too), and reaped. */
void stop_hub(AgentCompositor *ac) {
	end_conn(ac, true);
	if (ac->hub <= 0)
		return;
	if (!hub_exited_within(ac, 500)) {
		kill(-ac->hub, SIGTERM);
		if (!hub_exited_within(ac, 500)) {
			kill(-ac->hub, SIGKILL);
			waitpid(ac->hub, nullptr, 0);
			ac->hub = -1;
		}
	}
}

wb::Task<void> restart_after(AgentCompositor *ac, std::chrono::milliseconds delay) {
	co_await wb::sleep_for(*ac->server->main_sched, delay);
	/* (Only while `ac` lives: its scope's task.) */
	if (ac->hub <= 0 && !ac->given_up)
		start_hub(ac);
}

/* The hub exited, not asked to: restarted after a growing delay, unless
 * it crashed HubRestarts times within a minute. */
void hub_lost(AgentCompositor *ac) {
	end_conn(ac, false);
	auto now = std::chrono::steady_clock::now();
	ac->crashes.push_back(now);
	while (!ac->crashes.empty() && now - ac->crashes.front() > kHubRestartWindow)
		ac->crashes.pop_front();
	if (ac->crashes.size() >= kHubRestarts) {
		ac->given_up = true;
		wlr_log(WLR_ERROR, "agent tools: the hub crashed %zu times within %llds; not restarting "
				"it until agent tools are reconfigured", ac->crashes.size(),
				static_cast<long long>(kHubRestartWindow.count()));
		return;
	}
	auto delay = kHubRestartDelay * (1 << (ac->crashes.size() - 1));
	wlr_log(WLR_INFO, "agent tools: restarting the hub in %lld ms",
			static_cast<long long>(delay.count()));
	ac->restarts.spawn(restart_after(ac, delay));
}

}  // namespace

void agent_compositor_init(struct wb_server *server) {
	if (g_ac != nullptr)
		return;
	std::string command = hub_command();
	if (command.empty())
		return;
	auto *ac = new AgentCompositor;
	ac->server = server;
	ac->command = std::move(command);
	{
		std::lock_guard lock(ac->snap->mutex);
		ac->snap->locked = session_locked(server);
		ac->snap->away = ac->snap->locked;
	}
	g_ac = ac;
	start_hub(ac);
}

void agent_compositor_finish(struct wb_server *) {
	AgentCompositor *ac = g_ac;
	if (ac == nullptr)
		return;
	g_ac = nullptr;
	stop_hub(ac);
	delete ac;  /* (a restart waiting its delay is dropped with it) */
}

void agent_compositor_reconfigure(struct wb_server *server) {
	std::string command = hub_command();
	AgentCompositor *ac = g_ac;
	if (command.empty()) {
		if (ac != nullptr)
			wlr_log(WLR_INFO, "agent tools: turned off");
		agent_compositor_finish(server);
		return;
	}
	if (ac == nullptr) {
		agent_compositor_init(server);
		return;
	}
	/* On, and staying on: a hub given up on gets another go; a changed
	 * command takes effect with the next start. */
	ac->command = std::move(command);
	if (ac->given_up) {
		ac->given_up = false;
		ac->crashes.clear();
		if (ac->hub <= 0)
			start_hub(ac);
	}
}

void agent_compositor_state_changed(struct wb_server *server) {
	AgentCompositor *ac = g_ac;
	if (ac == nullptr)
		return;
	bool locked = session_locked(server);
	bool away = locked;  /* (idle past AwayAfter comes with the idle rules) */
	bool was_locked, was_away;
	{
		std::lock_guard lock(ac->snap->mutex);
		was_locked = ac->snap->locked;
		was_away = ac->snap->away;
		ac->snap->locked = locked;
		ac->snap->away = away;
	}
	if (locked != was_locked)
		emit_changed(ac->snap.get(), "Locked", locked);
	if (away != was_away)
		emit_changed(ac->snap.get(), "Away", away);
}

void agent_compositor_reap(struct wb_server *) {
	AgentCompositor *ac = g_ac;
	if (ac == nullptr || ac->hub <= 0)
		return;
	int status = 0;
	if (waitpid(ac->hub, &status, WNOHANG) == ac->hub) {
		if (WIFSIGNALED(status))
			wlr_log(WLR_ERROR, "agent tools: the hub was ended by signal %d", WTERMSIG(status));
		else
			wlr_log(WLR_ERROR, "agent tools: the hub exited (status %d)", WEXITSTATUS(status));
		ac->hub = -1;
		hub_lost(ac);
	}
}
