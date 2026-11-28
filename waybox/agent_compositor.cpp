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
#include <thread>
#include <vector>

extern char **environ;

#include "agent_compositor_xml.h"
#include "waybox/server.h"
#include "waybox/session_lock.h"
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

struct AgentCompositor {
	struct wb_server *server = nullptr;
	pid_t hub = -1;
	int fd = -1;  /* our end of the socket, until the thread takes it */
	std::shared_ptr<Snapshot> snap = std::make_shared<Snapshot>();
	GMainContext *ctx = nullptr;
	GMainLoop *loop = nullptr;
	std::jthread thread;
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

/* The D-Bus thread: the connection over our end of the socket, GLib's
 * loop until finish. */
void serve(AgentCompositor *ac, int fd, std::shared_ptr<wb::MainQueue> queue,
		std::shared_ptr<Snapshot> snap) {
	wb::block_signals();
	g_main_context_push_thread_default(ac->ctx);
	GError *err = nullptr;
	GSocket *sock = g_socket_new_from_fd(fd, &err);
	GDBusConnection *conn = nullptr;
	if (sock != nullptr) {
		GSocketConnection *stream = g_socket_connection_factory_create_connection(sock);
		g_object_unref(sock);
		gchar *guid = g_dbus_generate_guid();
		conn = g_dbus_connection_new_sync(G_IO_STREAM(stream), guid,
				G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, nullptr, nullptr, &err);
		g_free(guid);
		g_object_unref(stream);
	} else {
		close(fd);
	}
	if (conn == nullptr) {
		std::string why = err != nullptr ? err->message : "no socket";
		g_clear_error(&err);
		queue->post([why] {
			wlr_log(WLR_ERROR, "agent tools: the hub's connection failed: %s", why.c_str());
		});
		g_main_context_pop_thread_default(ac->ctx);
		return;
	}
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kAgentCompositorXml, &err);
	if (node == nullptr) {
		std::string why = err->message;
		g_clear_error(&err);
		queue->post([why] {
			wlr_log(WLR_ERROR, "agent tools: the interface's description: %s", why.c_str());
		});
	} else {
		static const GDBusInterfaceVTable vtable = {on_method, on_get, nullptr, {nullptr}};
		g_dbus_connection_register_object(conn, kPath, node->interfaces[0], &vtable,
				snap.get(), nullptr, &err);
		g_clear_error(&err);
		g_dbus_node_info_unref(node);
	}
	g_signal_connect(conn, "closed", G_CALLBACK(+[](GDBusConnection *, gboolean, GError *,
			gpointer q) {
		static_cast<wb::MainQueue *>(q)->post([] {
			wlr_log(WLR_INFO, "agent tools: the hub's connection closed");
		});
	}), queue.get());
	{
		std::lock_guard lock(snap->mutex);
		snap->conn = G_DBUS_CONNECTION(g_object_ref(conn));
	}
	g_main_loop_run(ac->loop);
	{
		std::lock_guard lock(snap->mutex);
		g_clear_object(&snap->conn);
	}
	g_dbus_connection_close_sync(conn, nullptr, nullptr);
	g_object_unref(conn);
	g_main_context_pop_thread_default(ac->ctx);
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
	ac->ctx = g_main_context_new();
	ac->loop = g_main_loop_new(ac->ctx, FALSE);
	{
		std::lock_guard lock(ac->snap->mutex);
		ac->snap->locked = session_locked(server);
		ac->snap->away = ac->snap->locked;
	}
	g_ac = ac;
	ac->thread = std::jthread(serve, ac, sv[0], server->main_queue, ac->snap);
	wlr_log(WLR_INFO, "agent tools: started the hub (%d)", static_cast<int>(pid));
}

void agent_compositor_finish(struct wb_server *) {
	AgentCompositor *ac = g_ac;
	if (ac == nullptr)
		return;
	g_ac = nullptr;
	g_main_loop_quit(ac->loop);  /* thread-safe; wakes the context */
	if (ac->thread.joinable())
		ac->thread.join();
	g_main_loop_unref(ac->loop);
	g_main_context_unref(ac->ctx);
	if (ac->hub > 0)
		kill(-ac->hub, SIGTERM);  /* it'd see its socket close; this is sooner */
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
