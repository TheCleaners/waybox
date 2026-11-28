/*
 * waybox-agent-hub: the agent hub (doc/agent-tools.md), started and
 * supervised by the compositor, which hands it the compositor interface
 * (org.freedesktop.AgentCompositor1) as a socket at launch: fd
 * AGENT_COMPOSITOR_FD, peer-to-peer D-Bus.
 *
 * So far: Hello, the state the compositor shares (locked, away, stopped)
 * and its changes, the hub's runtime folder, and the agent socket there
 * (agents.hpp), every agent at "safe" with no tools yet. It ends when the
 * compositor's connection closes. Providers and tools come with the steps
 * that bring them.
 */
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

#include "agents.hpp"
#include "audit.hpp"
#include "calls.hpp"
#include "providers.hpp"
#include "tunables.hpp"

namespace {

constexpr const char *kPath = "/org/freedesktop/AgentCompositor1";
constexpr const char *kIface = "org.freedesktop.AgentCompositor1";
constexpr guint32 kVersion = 1;
constexpr int kCallTimeoutMs = 5000;

struct Hub {
	GMainLoop *loop = nullptr;
	GDBusConnection *compositor = nullptr;
	bool locked = false, away = false, stopped = false;
	std::string provider;  /* the compositor's tools' manifest, from Hello */
};

void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	gchar *text = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_printerr("waybox-agent-hub: %s\n", text);
	g_free(text);
}

/* An fd the compositor handed over, named in the environment (which
 * children don't inherit). */
int inherited_fd(const char *var) {
	const char *v = getenv(var);
	if (v == nullptr || *v == '\0')
		return -1;
	char *end = nullptr;
	errno = 0;
	long fd = strtol(v, &end, 10);
	if (errno != 0 || *end != '\0' || fd < 0 || fd > 65535)
		return -1;
	unsetenv(var);
	if (fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC) < 0)
		return -1;
	return static_cast<int>(fd);
}

/* A folder only the user can enter: made so, or refused when it's there
 * and isn't (not a folder, someone else's). */
bool private_dir(const std::string &path) {
	if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
		say("%s: %s", path.c_str(), g_strerror(errno));
		return false;
	}
	struct stat st;
	if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
		say("%s: not a folder of this user's", path.c_str());
		return false;
	}
	if ((st.st_mode & 077) != 0 && chmod(path.c_str(), 0700) != 0) {
		say("%s: %s", path.c_str(), g_strerror(errno));
		return false;
	}
	return true;
}

/* $XDG_RUNTIME_DIR/agents/tools/: where the hub's sockets are; "" if it
 * can't be. */
std::string runtime_dir() {
	const char *run = g_getenv("XDG_RUNTIME_DIR");
	if (run == nullptr || *run != '/') {
		say("XDG_RUNTIME_DIR isn't set");
		return {};
	}
	std::string agents = std::string(run) + "/agents";
	if (!private_dir(agents) || !private_dir(agents + "/tools"))
		return {};
	return agents + "/tools";
}

bool get_bool(Hub &hub, const char *name, bool &out) {
	GError *err = nullptr;
	GVariant *r = g_dbus_connection_call_sync(hub.compositor, nullptr, kPath,
			"org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", kIface, name),
			G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &err);
	if (r == nullptr) {
		say("the compositor's %s: %s", name, err->message);
		g_clear_error(&err);
		return false;
	}
	GVariant *v = nullptr;
	g_variant_get(r, "(v)", &v);
	bool ok = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN);
	if (ok)
		out = g_variant_get_boolean(v);
	g_variant_unref(v);
	g_variant_unref(r);
	return ok;
}

void on_changed(GDBusConnection *, const char *, const char *, const char *, const char *,
		GVariant *params, gpointer data) {
	auto *hub = static_cast<Hub *>(data);
	const char *iface = nullptr;
	GVariantIter *changed = nullptr;
	g_variant_get(params, "(&sa{sv}as)", &iface, &changed, nullptr);
	if (g_strcmp0(iface, kIface) == 0) {
		const char *name;
		GVariant *value;
		while (g_variant_iter_loop(changed, "{&sv}", &name, &value)) {
			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
				continue;
			bool b = g_variant_get_boolean(value);
			if (g_strcmp0(name, "Locked") == 0) {
				hub->locked = b;
				wb::agent::agents_set_locked(b);
			}
			else if (g_strcmp0(name, "Away") == 0)
				hub->away = b;
			else if (g_strcmp0(name, "AssistantsStopped") == 0)
				hub->stopped = b;
			else
				continue;
			say("changed %s=%s", name, b ? "true" : "false");
		}
	}
	g_variant_iter_free(changed);
}

/* Connected, Hello said, the state read and followed. */
bool start(Hub &hub, int fd) {
	GError *err = nullptr;
	GSocket *sock = g_socket_new_from_fd(fd, &err);
	if (sock == nullptr) {
		close(fd);
		say("the compositor's socket: %s", err->message);
		g_clear_error(&err);
		return false;
	}
	GSocketConnection *stream = g_socket_connection_factory_create_connection(sock);
	g_object_unref(sock);
	hub.compositor = g_dbus_connection_new_sync(G_IO_STREAM(stream), nullptr,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &err);
	g_object_unref(stream);
	if (hub.compositor == nullptr) {
		say("the compositor's connection: %s", err->message);
		g_clear_error(&err);
		return false;
	}
	g_dbus_connection_set_exit_on_close(hub.compositor, FALSE);
	g_signal_connect(hub.compositor, "closed", G_CALLBACK(+[](GDBusConnection *, gboolean,
			GError *, gpointer data) {
		say("the compositor's connection closed");
		g_main_loop_quit(static_cast<Hub *>(data)->loop);
	}), &hub);
	g_dbus_connection_signal_subscribe(hub.compositor, nullptr, "org.freedesktop.DBus.Properties",
			"PropertiesChanged", kPath, kIface, G_DBUS_SIGNAL_FLAGS_NONE, on_changed, &hub,
			nullptr);

	GVariantBuilder mine;
	g_variant_builder_init(&mine, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&mine, "{sv}", "version", g_variant_new_string(WB_HUB_VERSION));
	g_variant_builder_add(&mine, "{sv}", "agents", g_variant_new_uint32(0));
	GVariant *r = g_dbus_connection_call_sync(hub.compositor, nullptr, kPath, kIface, "Hello",
			g_variant_new("(ua{sv})", kVersion, &mine), G_VARIANT_TYPE("(ua{sv})"),
			G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &err);
	if (r == nullptr) {
		say("Hello: %s", err->message);
		g_clear_error(&err);
		return false;
	}
	guint32 version = 0;
	GVariant *info = nullptr;
	g_variant_get(r, "(u@a{sv})", &version, &info);
	const char *name = "?";
	g_variant_lookup(info, "name", "&s", &name);
	const char *provider = nullptr;
	if (g_variant_lookup(info, "provider", "&s", &provider) && provider != nullptr)
		hub.provider = provider;
	say("connected to %s (compositor interface %u)", name, version);
	g_variant_unref(info);
	g_variant_unref(r);

	if (!get_bool(hub, "Locked", hub.locked) || !get_bool(hub, "Away", hub.away) ||
			!get_bool(hub, "AssistantsStopped", hub.stopped))
		return false;
	say("state locked=%s away=%s stopped=%s", hub.locked ? "true" : "false",
			hub.away ? "true" : "false", hub.stopped ? "true" : "false");
	return true;
}

/* The tunables: suggested values, then the distribution's settings
 * (each system folder, the least preferred first), then the user's. */
wb::agent::Tunables read_tunables() {
	wb::agent::Tunables t;
	std::vector<std::string> notes;
	auto apply = [&](const std::string &dir, bool system) {
		std::string path = dir + "/agents/tools/settings.ini";
		gchar *text = nullptr;
		gsize len = 0;
		if (!g_file_get_contents(path.c_str(), &text, &len, nullptr))
			return;
		std::vector<std::string> mine;
		wb::agent::apply_tunables(t, std::string_view(text, len), system, mine);
		g_free(text);
		for (const std::string &n : mine)
			say("%s: %s", path.c_str(), n.c_str());
	};
	std::vector<std::string> system;
	for (const char *const *d = g_get_system_config_dirs(); *d != nullptr; ++d)
		system.emplace_back(*d);
	for (auto it = system.rbegin(); it != system.rend(); ++it)
		apply(*it, true);
	apply(g_get_user_config_dir(), false);
	return t;
}

}  // namespace

int main() {
	int fd = inherited_fd("AGENT_COMPOSITOR_FD");
	int tools_fd = inherited_fd("AGENT_COMPOSITOR_TOOLS_FD");
	if (fd < 0) {
		say("no compositor interface (AGENT_COMPOSITOR_FD): the compositor starts the hub");
		return 2;
	}
	std::string dir = runtime_dir();
	if (dir.empty())
		return 1;
	Hub hub;
	hub.loop = g_main_loop_new(nullptr, FALSE);
	int status = 0;
	wb::agent::Tunables tunables = read_tunables();
	/* The audit: interrupted calls (a crash's) ended as outcome unknown,
	 * then the hub's start. */
	wb::agent::Audit audit(std::string(g_get_user_state_dir()) + "/agents/tools", tunables);
	if (!audit.ok())
		say("the audit can't be written: %s", audit.error().c_str());
	size_t recovered = audit.recover();
	audit.write(wb::json::Object{{"event", "hub"}, {"what", "started"},
			{"version", WB_HUB_VERSION}, {"recovered", static_cast<double>(recovered)}}, true);
	wb::agent::calls_init(tunables, &audit);
	wb::agent::agents_set_audit(&audit);
	if (start(hub, fd) && wb::agent::agents_listen(dir + "/agent", WB_HUB_VERSION, tunables)) {
		wb::agent::agents_set_locked(hub.locked);
		/* The compositor's own tools, if it offers any. */
		if (tools_fd >= 0 && !hub.provider.empty())
			wb::agent::providers_start_compositor(tools_fd, hub.provider, WB_HUB_VERSION, tunables);
		else if (tools_fd >= 0)
			close(tools_fd);
		g_main_loop_run(hub.loop);
	} else {
		status = 1;
	}
	wb::agent::agents_stop();
	wb::agent::providers_stop();
	wb::agent::calls_stop();
	if (hub.compositor != nullptr) {
		g_dbus_connection_close_sync(hub.compositor, nullptr, nullptr);
		g_object_unref(hub.compositor);
	}
	g_main_loop_unref(hub.loop);
	return status;
}
