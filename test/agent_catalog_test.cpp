#include "wb_test.hpp"

#include "../agent/catalog.hpp"
#include "../agent/mcp_agent.hpp"
#include "../agent/mcp_client.hpp"

using namespace wb;
using namespace wb::agent;

namespace {

const char *kManifest =
		"[Provider]\nVersion=1\nId=org.waybox.Waybox\nName=Waybox\n"
		"[Tool list_windows]\nSensitivity=read\nDescription=The windows.\n"
		"[Tool window_titles]\nSensitivity=content\nDescription=Titles.\n"
		"[Tool get_version]\nSensitivity=read\nUntrusted=no\nDescription=Version.\n"
		"[Tool close_window]\nSensitivity=destroy\nDescription=Close.\n"
		"[Tool fetch]\nSensitivity=read\nReach=outside\nDescription=Fetch.\n"
		"[Tool weird]\nSensitivity=telepathy\nDescription=?\n";

json::Array live(std::initializer_list<const char *> names) {
	json::Array a;
	for (const char *n : names)
		a.push_back(json::Object{{"name", n}, {"description", "the provider's own words"},
				{"inputSchema", json::Object{{"type", "object"}}}});
	return a;
}

const OfferedTool *named(const std::vector<OfferedTool> &v, const std::string &n) {
	for (const OfferedTool &t : v)
		if (t.name == n)
			return &t;
	return nullptr;
}

}  // namespace

WB_TEST(manifest_read) {
	Manifest m = read_manifest(kManifest);
	WB_CHECK(m.error.empty() && m.id == "org.waybox.Waybox" && m.tools.size() == 6);
	WB_CHECK(m.tools["list_windows"].untrusted && !m.tools["get_version"].untrusted);
	WB_CHECK(m.tools["fetch"].reach == "outside");
	WB_CHECK(!read_manifest("[Provider]\nName=x\n").error.empty());
	WB_CHECK(!read_manifest("[Provider]\nId=a.b\nVersion=2\n").error.empty());
}

WB_TEST(names) {
	WB_CHECK(short_name("org.waybar.Waybar") == "waybar");
	WB_CHECK(short_name("org.example.My App") == "my_app");
	WB_CHECK(short_name("com.x.ABCDEFGHIJKLMNOPQRST") == "abcdefghijklmnop");
	WB_CHECK(offered_name("waybox", "list_windows") == "waybox__list_windows");
	/* `__` only as the separator. */
	WB_CHECK(offered_name("waybox", "a__b") == "waybox__a_b");
	WB_CHECK(offered_name("waybox", std::string(60, 'x')).empty());
}

WB_TEST(merged_with_the_manifest) {
	std::vector<std::string> notes;
	auto offered = merge_tools(read_manifest(kManifest),
			live({"list_windows", "get_version", "not_declared"}), notes, true);
	WB_CHECK(offered.size() == 2 && notes.size() == 1);
	const OfferedTool *lw = named(offered, "waybox__list_windows");
	WB_CHECK(lw != nullptr && lw->tool == "list_windows" && lw->provider == "org.waybox.Waybox");
	/* The manifest's description, never the provider's live one. */
	WB_CHECK(lw->definition.at("description").string() &&
			*lw->definition.at("description").string() == "The windows.");
	const json::Value &ann = lw->definition.at("annotations");
	WB_CHECK(*ann.get("readOnlyHint")->boolean() && !*ann.get("destructiveHint")->boolean() &&
			!*ann.get("openWorldHint")->boolean());
}

WB_TEST(hints_contradicting_refused) {
	Manifest m = read_manifest(kManifest);
	json::Array l = live({"list_windows", "close_window"});
	l[0] = json::Object{{"name", "list_windows"},
			{"annotations", json::Object{{"readOnlyHint", false}}}};
	std::vector<std::string> notes;
	auto offered = merge_tools(m, l, notes, true);
	WB_CHECK(offered.size() == 1 && offered[0].tool == "close_window");
	WB_CHECK(notes.size() == 1 && notes[0].find("contradict") != std::string::npos);
	const json::Value &ann = offered[0].definition.at("annotations");
	WB_CHECK(!*ann.get("readOnlyHint")->boolean() && *ann.get("destructiveHint")->boolean());
	/* An outside read is open-world. */
	auto f = merge_tools(m, live({"fetch"}), notes, true);
	WB_CHECK(*f[0].definition.at("annotations").get("openWorldHint")->boolean());
}

WB_TEST(no_impersonation) {
	/* Another provider calling itself Waybox gets no tools under its name. */
	Manifest evil = read_manifest(
			"[Provider]\nId=com.evil.Waybox\n[Tool window_titles]\nSensitivity=content\n");
	std::vector<std::string> notes;
	WB_CHECK(merge_tools(evil, live({"window_titles"}), notes, false).empty());
	WB_CHECK(notes.size() == 1 && notes[0].find("reserved") != std::string::npos);
	WB_CHECK(reserved_short_name("desktop", true) && reserved_short_name("ui", false));
	WB_CHECK(!reserved_short_name("waybox", true) && !reserved_short_name("waybar", false));
}

WB_TEST(levels) {
	std::vector<std::string> notes;
	auto offered = merge_tools(read_manifest(kManifest),
			live({"list_windows", "window_titles", "close_window", "weird"}), notes, true);
	auto level = [&](const char *preset, const char *name) {
		return preset_level(preset, *named(offered, name));
	};
	WB_CHECK(level("safe", "waybox__list_windows") == Level::Allow);
	/* In safe, content only for the compositor's window titles. */
	WB_CHECK(level("safe", "waybox__window_titles") == Level::Allow);
	WB_CHECK(level("safe", "waybox__close_window") == Level::Off);
	WB_CHECK(level("trusted", "waybox__close_window") == Level::Ask);
	WB_CHECK(level("full", "waybox__close_window") == Level::Allow);
	/* An unknown sensitivity: off, whatever the preset. */
	WB_CHECK(level("full", "waybox__weird") == Level::Off);
	/* The same content tool from another provider: off in safe. */
	OfferedTool other = *named(offered, "waybox__window_titles");
	other.compositor = false;
	WB_CHECK(preset_level("safe", other) == Level::Off);
}

namespace {

std::string answer(const std::string &id, const std::string &result) {
	return R"({"jsonrpc":"2.0","id":")" + id + R"(","result":)" + result + "}";
}

std::string id_of(const std::string &line) {
	return json::parse(line)->str("id");
}

std::string method_of(const std::string &line) {
	return json::parse(line)->str("method");
}

}  // namespace

WB_TEST(client_stateless) {
	McpClient c("hub", "1");
	auto out = c.start();
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "server/discover");
	out = c.receive(answer("discover",
			R"({"resultType":"complete","supportedVersions":["2026-07-28"],"capabilities":{}})"));
	WB_CHECK(c.era() == McpClient::Era::Stateless && out.size() == 2);
	WB_CHECK(method_of(out[0]) == "subscriptions/listen" && method_of(out[1]) == "tools/list");
	/* Every stateless request names its revision. */
	WB_CHECK(json::parse(out[1])->get("params")->get("_meta")->str(
			"io.modelcontextprotocol/protocolVersion") == "2026-07-28");
	/* Two pages. */
	out = c.receive(answer(id_of(out[1]), R"({"tools":[{"name":"a"}],"nextCursor":"p2"})"));
	WB_CHECK(out.size() == 1 && json::parse(out[0])->get("params")->str("cursor") == "p2");
	WB_CHECK(c.state() != McpClient::State::Ready);
	out = c.receive(answer(id_of(out[0]), R"({"tools":[{"name":"b"}]})"));
	WB_CHECK(c.state() == McpClient::State::Ready && c.tools().size() == 2);
	WB_CHECK(c.take_tools_changed() && !c.take_tools_changed());
	/* A call, answered. */
	bool got = false;
	out = c.call("a", json::Object{{"x", 1.0}}, [&](bool ok, const json::Object &body) {
		got = ok && body.count("content") == 1;
	});
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "tools/call");
	c.receive(answer(c.last_call(), R"({"content":[]})"));
	WB_CHECK(got);
}

WB_TEST(client_falls_back_to_initialize) {
	McpClient c("hub", "1");
	c.start();
	/* An older server: method not found. */
	auto out = c.receive(R"({"jsonrpc":"2.0","id":"discover","error":{"code":-32601,"message":"no"}})");
	WB_CHECK(c.era() == McpClient::Era::Older && out.size() == 1 && method_of(out[0]) == "initialize");
	out = c.receive(answer("initialize", R"({"protocolVersion":"2025-11-25","capabilities":{}})"));
	WB_CHECK(out.size() == 2 && method_of(out[0]) == "notifications/initialized" &&
			method_of(out[1]) == "tools/list");
	/* No _meta revision in the older era. */
	WB_CHECK(json::parse(out[1])->get("params")->get("_meta") == nullptr);
	c.receive(answer(id_of(out[1]), R"({"tools":[{"name":"a"}]})"));
	WB_CHECK(c.state() == McpClient::State::Ready && c.take_tools_changed());
	/* The older era's change notification: listed again. */
	out = c.receive(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "tools/list");
	/* No answer to discover at all: initialize too. */
	McpClient quiet("hub", "1");
	quiet.start();
	out = quiet.timed_out();
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "initialize");
	WB_CHECK(quiet.timed_out().empty());
}

WB_TEST(client_calls_end_with_the_connection) {
	McpClient c("hub", "1");
	c.start();
	auto out = c.receive(answer("discover", R"({"supportedVersions":["2026-07-28"]})"));
	c.receive(answer(id_of(out[1]), R"({"tools":[]})"));
	int failed = 0;
	c.call("a", {}, [&](bool ok, const json::Object &) { failed += !ok; });
	std::string first = c.last_call();
	c.call("b", {}, [&](bool ok, const json::Object &) { failed += !ok; });
	/* Cancelled: the provider is asked; its answer still comes (here: the
	 * connection goes, so both end, marked lost). */
	out = c.cancel(first);
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "notifications/cancelled");
	c.lost("gone");
	WB_CHECK(failed == 2 && c.state() == McpClient::State::Failed);
	/* Not ready: a call fails at once. */
	bool refused = false;
	WB_CHECK(c.call("a", {}, [&](bool ok, const json::Object &) { refused = !ok; }).empty() && refused);
	/* A modern refusal of every revision: failed, not the older era. */
	McpClient m("hub", "1");
	m.start();
	m.receive(R"({"jsonrpc":"2.0","id":"discover","error":{"code":-32022,"message":"no","data":{"supported":["2027-01-01"]}}})");
	WB_CHECK(m.state() == McpClient::State::Failed);
}

namespace {

/* A source that answers later, by hand. */
struct Later : ToolSource {
	std::vector<json::Value> pending;
	std::vector<json::Value> cancelled;
	json::Array list_tools() override { return json::Array{json::Object{{"name", "t"}}}; }
	void call(McpServer &, const json::Value &id, const std::string &, const json::Object &) override {
		pending.push_back(id);
	}
	void cancel(const json::Value &id) override { cancelled.push_back(id); }
};

const std::string kMeta = R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}})";

}  // namespace

WB_TEST(server_answers_later) {
	Later src;
	McpServer s("hub", "1", 8, &src, "Be careful.");
	auto out = s.receive(R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{)" + kMeta + "}}");
	WB_CHECK(json::parse(out[0])->get("result")->str("instructions") == "Be careful.");
	out = s.receive(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"t",)" + kMeta + "}}");
	WB_CHECK(out.empty() && src.pending.size() == 1 && s.calls_under_way() == 1);
	/* The same id again while it's under way: refused. */
	out = s.receive(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"t",)" + kMeta + "}}");
	WB_CHECK(out.size() == 1 && json::parse(out[0])->get("error") != nullptr);
	s.complete(src.pending[0], json::Object{{"content", json::Array{}}});
	out = s.take_output();
	WB_CHECK(out.size() == 1 && json::parse(out[0])->get("result")->str("resultType") == "complete");
	/* Answered once only. */
	s.complete(src.pending[0], json::Object{});
	WB_CHECK(s.take_output().empty() && s.calls_under_way() == 0);
	/* Cancelled by the client: the source is told, and no answer goes out. */
	s.receive(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"t",)" + kMeta + "}}");
	s.receive(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3}})");
	WB_CHECK(src.cancelled.size() == 1);
	s.fail(json::Value(3.0), -32603, "late");
	WB_CHECK(s.take_output().empty());
	/* Arguments must be an object. */
	out = s.receive(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"t","arguments":[1],)" + kMeta + "}}");
	WB_CHECK(json::parse(out[0])->get("error") != nullptr);
}

WB_TEST(server_tools_changed) {
	Later src;
	McpServer older("hub", "1", 8, &src);
	older.receive(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
	older.tools_changed();
	auto out = older.take_output();
	WB_CHECK(out.size() == 1 && method_of(out[0]) == "notifications/tools/list_changed");
	McpServer stateless("hub", "1", 8, &src);
	stateless.receive(R"({"jsonrpc":"2.0","id":"l1","method":"subscriptions/listen","params":{)" + kMeta +
			R"(,"notifications":{"toolsListChanged":true}}})");
	stateless.receive(R"({"jsonrpc":"2.0","id":"l2","method":"subscriptions/listen","params":{)" + kMeta +
			R"(,"notifications":{}}})");
	stateless.tools_changed();
	out = stateless.take_output();
	/* Only on the stream that asked for it, with its id. */
	WB_CHECK(out.size() == 1 && json::parse(out[0])->get("params")->get("_meta")->str(
			"io.modelcontextprotocol/subscriptionId") == "l1");
}
