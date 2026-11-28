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

#include <cstdlib>
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
#include "waybox/glib_thread.hpp"
#include "waybox/threads.hpp"

namespace {

constexpr const char *kPath = "/org/freedesktop/AgentCompositor1";
constexpr const char *kIface = "org.freedesktop.AgentCompositor1";
constexpr guint32 kVersion = 1;

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
	~Conn() { g_object_unref(cancel); }
};

struct AgentCompositor {
	struct wb_server *server = nullptr;
	pid_t hub = -1;
	std::shared_ptr<Snapshot> snap = std::make_shared<Snapshot>();
	std::shared_ptr<Conn> conn = std::make_shared<Conn>();
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
		if (auto c = static_cast<std::weak_ptr<Conn> *>(d)->lock())
			log_on_main(c, "the hub's connection closed", WLR_INFO);
	}), closed_data, [](gpointer d, GClosure *) {
		delete static_cast<std::weak_ptr<Conn> *>(d);
	}, GConnectFlags(0));
	std::lock_guard lock(c->snap->mutex);
	c->snap->conn = G_DBUS_CONNECTION(g_object_ref(conn));
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
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, nullptr, c->cancel, on_connected,
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

void agent_compositor_init(struct wb_server *server) {
	const char *hub = getenv("WAYBOX_AGENT_HUB");
	if (hub == nullptr || *hub == '\0' || g_ac != nullptr)
		return;
	/* Everything the child needs, made before fork: between fork and exec
	 * only async-signal-safe calls (Waybox has threads). */
	std::vector<std::string> env_store;
	for (char **e = environ; *e != nullptr; ++e)
		if (std::string(*e).rfind("AGENT_COMPOSITOR_FD=", 0) != 0)
			env_store.emplace_back(*e);
	env_store.emplace_back("AGENT_COMPOSITOR_FD=3");
	std::vector<char *> envp;
	for (auto &e : env_store)
		envp.push_back(e.data());
	envp.push_back(nullptr);
	const char *argv[] = {"sh", "-c", hub, nullptr};
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		wlr_log_errno(WLR_ERROR, "agent tools: socketpair");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "agent tools: fork");
		close(sv[0]);
		close(sv[1]);
		return;
	}
	if (pid == 0) {
		/* The hub: its end as fd 3, named in its environment. */
		setpgid(0, 0);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		if (sv[1] == 3) {
			if (fcntl(3, F_SETFD, 0) < 0)  /* (dup2 to itself keeps CLOEXEC) */
				_exit(127);
		} else if (dup2(sv[1], 3) < 0) {
			_exit(127);
		}
		execve("/bin/sh", const_cast<char *const *>(argv), envp.data());
		_exit(127);
	}
	setpgid(pid, pid);
	close(sv[1]);
	auto *ac = new AgentCompositor;
	ac->server = server;
	ac->hub = pid;
	{
		std::lock_guard lock(ac->snap->mutex);
		ac->snap->locked = session_locked(server);
		ac->snap->away = ac->snap->locked;
	}
	ac->conn->snap = ac->snap;
	ac->conn->queue = server->main_queue;
	g_ac = ac;
	int fd = sv[0];
	server->glib->post([c = ac->conn, fd] { connect_hub(c, fd); });
	wlr_log(WLR_INFO, "agent tools: started the hub (%d)", static_cast<int>(pid));
}

void agent_compositor_finish(struct wb_server *) {
	AgentCompositor *ac = g_ac;
	if (ac == nullptr)
		return;
	g_ac = nullptr;
	/* Ended on the GLib thread, waited for: nothing of it outlives this. */
	std::latch ended(1);
	ac->server->glib->post([c = ac->conn, &ended] {
		disconnect_hub(c);
		ended.count_down();
	});
	ended.wait();
	/* The hub sees its socket close and ends by itself; one that doesn't,
	 * within a moment, is ended (its process group: what it started too). */
	for (int i = 0; ac->hub > 0 && i < 50; ++i) {
		if (waitpid(ac->hub, nullptr, WNOHANG) == ac->hub) {
			ac->hub = -1;
			break;
		}
		usleep(10000);
	}
	if (ac->hub > 0)
		kill(-ac->hub, SIGTERM);
	delete ac;
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
		wlr_log(WLR_INFO, "agent tools: the hub exited (%d)", status);
		ac->hub = -1;
	}
}
