/*
 * The hub's providers (providers.hpp).
 */
#include "providers.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <cstdarg>
#include <deque>
#include <map>
#include <memory>

namespace wb::agent {

namespace {

/* How long a provider has to answer server/discover before the older era
 * is tried (as MCP recommends: "a moment"). */
constexpr guint kDiscoverWaitMs = 2000;
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

struct Provider {
	std::string id;
	Manifest manifest;
	GSocketConnection *sc = nullptr;
	GCancellable *cancel = g_cancellable_new();
	std::unique_ptr<McpClient> client;
	std::string buf;
	std::deque<std::shared_ptr<std::string>> outq;
	bool writing = false;
	guint discover_wait = 0;
	size_t max_frame = 0;
	std::map<unsigned, std::string> calls;  /* handle → the client's call id */
	~Provider() {
		if (discover_wait != 0)
			g_source_remove(discover_wait);
		g_cancellable_cancel(cancel);
		if (sc != nullptr) {
			g_io_stream_close(G_IO_STREAM(sc), nullptr, nullptr);
			g_object_unref(sc);
		}
		g_object_unref(cancel);
	}
};

Provider *g_compositor = nullptr;
std::vector<OfferedTool> g_offered;
std::function<void()> g_on_change;
unsigned g_next_handle = 1;

void send_lines(Provider *p, std::vector<std::string> lines);

void write_next(Provider *p) {
	if (p->outq.empty()) {
		p->writing = false;
		return;
	}
	p->writing = true;
	auto data = p->outq.front();
	p->outq.pop_front();
	auto *keep = new std::shared_ptr<std::string>(data);
	g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(p->sc)),
			data->data(), data->size(), G_PRIORITY_DEFAULT, p->cancel,
			[](GObject *src, GAsyncResult *res, gpointer d) {
				std::unique_ptr<std::shared_ptr<std::string>> keep(
						static_cast<std::shared_ptr<std::string> *>(d));
				GError *err = nullptr;
				bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, nullptr, &err);
				bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
				g_clear_error(&err);
				if (cancelled || g_compositor == nullptr)
					return;
				if (!ok) {
					g_compositor->client->lost("the provider's socket closed");
					return;
				}
				write_next(g_compositor);
			}, keep);
}

void send_lines(Provider *p, std::vector<std::string> lines) {
	for (std::string &l : lines) {
		l += '\n';
		p->outq.push_back(std::make_shared<std::string>(std::move(l)));
	}
	if (!p->writing)
		write_next(p);
}

/* The client's state may have moved on: a new tool list is merged with
 * the manifest and offered. */
void settle(Provider *p) {
	if (p->discover_wait != 0 && p->client->state() != McpClient::State::Discovering) {
		g_source_remove(p->discover_wait);
		p->discover_wait = 0;
	}
	if (p->client->state() == McpClient::State::Failed) {
		say("%s: %s", p->id.c_str(), p->client->error().c_str());
		return;
	}
	if (!p->client->take_tools_changed())
		return;
	std::vector<std::string> notes;
	g_offered = merge_tools(p->manifest, p->client->tools(), notes, true);
	for (const std::string &n : notes)
		say("%s: %s", p->id.c_str(), n.c_str());
	say("%s: %zu tools (MCP %s)", p->id.c_str(), g_offered.size(),
			p->client->era() == McpClient::Era::Stateless ? "2026-07-28" : "with initialize");
	if (g_on_change)
		g_on_change();
}

void read_more(Provider *p);

void on_read(GObject *src, GAsyncResult *res, gpointer) {
	GError *err = nullptr;
	GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
	bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&err);
	if (cancelled || g_compositor == nullptr) {
		if (bytes != nullptr)
			g_bytes_unref(bytes);
		return;
	}
	Provider *p = g_compositor;
	gsize n = 0;
	const char *data = bytes != nullptr ? static_cast<const char *>(g_bytes_get_data(bytes, &n)) : nullptr;
	if (n == 0) {
		if (bytes != nullptr)
			g_bytes_unref(bytes);
		say("%s: its connection closed", p->id.c_str());
		p->client->lost("the provider went away");
		g_offered.clear();
		if (g_on_change)
			g_on_change();
		return;
	}
	p->buf.append(data, n);
	g_bytes_unref(bytes);
	size_t start = 0;
	for (size_t nl; (nl = p->buf.find('\n', start)) != std::string::npos; start = nl + 1) {
		std::string_view line(p->buf.data() + start, nl - start);
		if (!line.empty())
			send_lines(p, p->client->receive(line));
		if (g_compositor != p)
			return;
		settle(p);
	}
	p->buf.erase(0, start);
	if (p->buf.size() > p->max_frame) {
		say("%s: a message longer than MaxFrame: closed", p->id.c_str());
		p->client->lost("a message longer than MaxFrame");
		g_offered.clear();
		if (g_on_change)
			g_on_change();
		return;
	}
	read_more(p);
}

void read_more(Provider *p) {
	g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(p->sc)), kReadChunk,
			G_PRIORITY_DEFAULT, p->cancel, on_read, nullptr);
}

/* The manifest `id`, from the user's data folder, else the system's. */
Manifest find_manifest(const std::string &id) {
	std::vector<std::string> dirs = {g_get_user_data_dir()};
	for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; ++d)
		dirs.emplace_back(*d);
	for (const std::string &dir : dirs) {
		std::string path = dir + "/agents/tools/" + id + ".ini";
		gchar *text = nullptr;
		gsize len = 0;
		if (!g_file_get_contents(path.c_str(), &text, &len, nullptr))
			continue;
		Manifest m = read_manifest(std::string_view(text, len));
		g_free(text);
		if (m.error.empty() && m.id != id)
			m.error = "its Id is " + m.id + ", not " + id;
		if (!m.error.empty())
			m.error = path + ": " + m.error;
		return m;
	}
	Manifest none;
	none.error = "no manifest " + id + ".ini in the data folders' agents/tools/";
	return none;
}

}  // namespace

void providers_start_compositor(int fd, const std::string &id, const std::string &hub_version,
		const Tunables &t) {
	providers_stop();
	Manifest m = find_manifest(id);
	if (!m.error.empty()) {
		say("the compositor's tools: %s: none offered", m.error.c_str());
		close(fd);
		return;
	}
	GError *err = nullptr;
	GSocket *sock = g_socket_new_from_fd(fd, &err);
	if (sock == nullptr) {
		close(fd);
		say("the compositor's tools socket: %s", err->message);
		g_clear_error(&err);
		return;
	}
	auto *p = new Provider;
	p->id = id;
	p->manifest = std::move(m);
	p->max_frame = t.max_frame;
	p->sc = g_socket_connection_factory_create_connection(sock);
	g_object_unref(sock);
	p->client = std::make_unique<McpClient>("waybox-agent-hub", hub_version);
	g_compositor = p;
	p->discover_wait = g_timeout_add(kDiscoverWaitMs, [](gpointer) -> gboolean {
		if (g_compositor != nullptr) {
			g_compositor->discover_wait = 0;
			send_lines(g_compositor, g_compositor->client->timed_out());
		}
		return G_SOURCE_REMOVE;
	}, nullptr);
	send_lines(p, p->client->start());
	read_more(p);
}

void providers_stop() {
	Provider *p = g_compositor;
	g_compositor = nullptr;
	if (p != nullptr) {
		p->client->lost("the hub is ending");
		delete p;
	}
	g_offered.clear();
}

const std::vector<OfferedTool> &offered_tools() {
	return g_offered;
}

void providers_on_change(std::function<void()> f) {
	g_on_change = std::move(f);
}

unsigned providers_call(const OfferedTool &t, const json::Object &args, McpClient::Done done) {
	Provider *p = g_compositor;
	if (p == nullptr || t.provider != p->id) {
		done(false, json::Object{{"code", -32603.0}, {"message", "its provider isn't connected"}});
		return 0;
	}
	unsigned handle = g_next_handle++;
	auto wrapped = [handle, done = std::move(done)](bool ok, const json::Object &body) {
		if (g_compositor != nullptr)
			g_compositor->calls.erase(handle);
		done(ok, body);
	};
	std::vector<std::string> lines = p->client->call(t.tool, args, std::move(wrapped));
	if (lines.empty())
		return 0;  /* (answered already: not ready) */
	p->calls[handle] = p->client->last_call();
	send_lines(p, std::move(lines));
	return handle;
}

void providers_cancel(unsigned handle) {
	Provider *p = g_compositor;
	if (p == nullptr)
		return;
	auto it = p->calls.find(handle);
	if (it == p->calls.end())
		return;
	std::string call_id = it->second;
	p->calls.erase(it);
	send_lines(p, p->client->cancel(call_id));
}

}  // namespace wb::agent
