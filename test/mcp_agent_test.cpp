#include "wb_test.hpp"

#include "../agent/mcp_agent.hpp"

using namespace wb;
using agent::AgentEndpoint;

namespace {

json::Value one(AgentEndpoint &ep, const std::string &line) {
	auto out = ep.receive(line);
	if (out.size() != 1)
		return json::Value(nullptr);
	auto v = json::parse(out[0]);
	return v ? *v : json::Value(nullptr);
}

double code_of(const json::Value &v) {
	const json::Value *e = v.get("error");
	const json::Value *c = e != nullptr ? e->get("code") : nullptr;
	return c != nullptr && c->number() != nullptr ? *c->number() : 0;
}

const json::Value *result_of(const json::Value &v) { return v.get("result"); }

std::string req(int id, const std::string &method, const std::string &params = "{}") {
	return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" + method +
			"\",\"params\":" + params + "}";
}

const std::string kMeta2026 =
		"\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
		"\"io.modelcontextprotocol/clientCapabilities\":{},"
		"\"io.modelcontextprotocol/clientInfo\":{\"name\":\"pi\",\"version\":\"1\"}}";

}  // namespace

WB_TEST(handshake_read_and_answered) {
	auto hs = agent::read_handshake(
			R"({"agent-tools":1,"token":"abc","lower":{"exec":"off"},"label":"laptop"})");
	WB_CHECK(hs.ok && hs.token && *hs.token == "abc" && hs.label == "laptop");
	WB_CHECK(hs.lower.count("exec") == 1);
	auto a = json::parse(agent::handshake_answer(hs, "safe", "waybox-agent-hub 0.3"));
	WB_CHECK(a && a->str("profile") == "safe" && a->str("hub") == "waybox-agent-hub 0.3");
	WB_CHECK(a->get("ok")->boolean() && *a->get("ok")->boolean());
	/* The token never comes back. */
	WB_CHECK(agent::handshake_answer(hs, "safe", "h").find("abc") == std::string::npos);
	/* No token at all: fine (the hub gives "safe"). */
	auto bare = agent::read_handshake(R"({"agent-tools":1})");
	WB_CHECK(bare.ok && !bare.token);
}

WB_TEST(handshake_refused) {
	for (const char *line : {"not json", "[]", R"({"label":"x"})", R"({"agent-tools":2})",
			R"({"agent-tools":1,"token":5})", R"({"agent-tools":1,"lower":"exec=off"})"}) {
		auto hs = agent::read_handshake(line);
		WB_CHECK(!hs.ok && !hs.error.empty());
	}
	auto a = json::parse(agent::handshake_answer(agent::read_handshake(R"({"agent-tools":2})"),
			"safe", "h"));
	WB_CHECK(a && !*a->get("ok")->boolean() && a->get("versions") != nullptr);
	WB_CHECK(a->get("profile") == nullptr);
}

WB_TEST(older_era) {
	AgentEndpoint ep("waybox-agent-hub", "0.3");
	/* Before initialize: refused. */
	WB_CHECK(code_of(one(ep, req(1, "tools/list"))) == -32600);
	WB_CHECK(ep.era() == AgentEndpoint::Era::Unknown);
	auto init = one(ep, req(2, "initialize",
			R"({"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"claude-code","version":"2"}})"));
	const json::Value *r = result_of(init);
	WB_CHECK(r != nullptr && r->str("protocolVersion") == "2025-11-25");
	WB_CHECK(r->get("capabilities")->get("tools")->get("listChanged") != nullptr);
	WB_CHECK(r->get("serverInfo")->str("name") == "waybox-agent-hub");
	WB_CHECK(ep.era() == AgentEndpoint::Era::Older && ep.claimed_name() == "claude-code");
	/* The id comes back as sent. */
	WB_CHECK(init.get("id")->number() && *init.get("id")->number() == 2);
	/* initialized is a notification: no answer. */
	WB_CHECK(ep.receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
	auto list = one(ep, req(3, "tools/list"));
	WB_CHECK(result_of(list) && result_of(list)->get("tools")->array()->empty());
	/* No resultType in the older era. */
	WB_CHECK(result_of(list)->get("resultType") == nullptr);
	WB_CHECK(code_of(one(ep, req(4, "tools/call", R"({"name":"list_windows"})"))) == -32602);
	WB_CHECK(result_of(one(ep, req(5, "ping"))) != nullptr);
	WB_CHECK(code_of(one(ep, req(6, "resources/list"))) == -32601);
	WB_CHECK(code_of(one(ep, req(7, "initialize", R"({"protocolVersion":"2025-11-25"})"))) == -32600);
}

WB_TEST(older_revisions_negotiated) {
	{
		AgentEndpoint ep("h", "1");
		auto r = one(ep, req(1, "initialize", R"({"protocolVersion":"2025-06-18"})"));
		WB_CHECK(result_of(r) && result_of(r)->str("protocolVersion") == "2025-06-18");
	}
	{
		/* Newer than known, not stateless: the newest older one. */
		AgentEndpoint ep("h", "1");
		auto r = one(ep, req(1, "initialize", R"({"protocolVersion":"2026-01-15"})"));
		WB_CHECK(result_of(r) && result_of(r)->str("protocolVersion") == "2025-11-25");
	}
	{
		/* Too old: refused, naming the supported ones; it may try again. */
		AgentEndpoint ep("h", "1");
		auto r = one(ep, req(1, "initialize", R"({"protocolVersion":"2024-11-05"})"));
		WB_CHECK(code_of(r) == -32602);
		const json::Value *sup = r.get("error")->get("data")->get("supported");
		WB_CHECK(sup != nullptr && sup->array()->size() == 2);
		WB_CHECK(r.get("error")->get("data")->str("requested") == "2024-11-05");
		auto again = one(ep, req(2, "initialize", R"({"protocolVersion":"2025-11-25"})"));
		WB_CHECK(result_of(again) != nullptr);
	}
}

WB_TEST(stateless_era) {
	AgentEndpoint ep("waybox-agent-hub", "0.3");
	auto d = one(ep, req(1, "server/discover", "{" + kMeta2026 + "}"));
	const json::Value *r = result_of(d);
	WB_CHECK(r != nullptr && ep.era() == AgentEndpoint::Era::Stateless);
	WB_CHECK(r->str("resultType") == "complete" && r->str("cacheScope") == "private");
	WB_CHECK(r->get("ttlMs") != nullptr && r->get("supportedVersions")->array()->size() == 1);
	WB_CHECK(r->get("_meta")->get("io.modelcontextprotocol/serverInfo")->str("name") ==
			"waybox-agent-hub");
	WB_CHECK(ep.claimed_name() == "pi");
	auto list = one(ep, req(2, "tools/list", "{" + kMeta2026 + "}"));
	WB_CHECK(result_of(list) && result_of(list)->get("tools")->array()->empty());
	WB_CHECK(result_of(list)->str("resultType") == "complete" &&
			result_of(list)->str("cacheScope") == "private");
	/* Each request names its revision. */
	WB_CHECK(code_of(one(ep, req(3, "tools/list"))) == -32602);
	auto bad = one(ep, req(4, "tools/list",
			R"({"_meta":{"io.modelcontextprotocol/protocolVersion":"2027-01-01"}})"));
	WB_CHECK(code_of(bad) == -32022);
	WB_CHECK(bad.get("error")->get("data")->str("requested") == "2027-01-01");
	/* initialize isn't of this era. */
	WB_CHECK(code_of(one(ep, req(5, "initialize", "{" + kMeta2026 + "}"))) == -32601);
}

WB_TEST(stateless_listen_acknowledged) {
	AgentEndpoint ep("h", "1");
	auto out = ep.receive(R"({"jsonrpc":"2.0","id":"listen-1","method":"subscriptions/listen","params":{)" +
			kMeta2026 + R"(,"notifications":{"toolsListChanged":true,"promptsListChanged":true}}})");
	/* The acknowledgement only: the stream stays open, no response. */
	WB_CHECK(out.size() == 1);
	auto ack = json::parse(out[0]);
	WB_CHECK(ack && ack->str("method") == "notifications/subscriptions/acknowledged");
	WB_CHECK(ack->get("id") == nullptr);
	const json::Value *p = ack->get("params");
	WB_CHECK(p->get("_meta")->str("io.modelcontextprotocol/subscriptionId") == "listen-1");
	/* Only what's honored: tool-list changes, not prompts. */
	WB_CHECK(p->get("notifications")->get("toolsListChanged") != nullptr);
	WB_CHECK(p->get("notifications")->get("promptsListChanged") == nullptr);
}

WB_TEST(first_request_settles_nothing_else) {
	AgentEndpoint ep("h", "1");
	/* Neither initialize nor a revision in _meta. */
	WB_CHECK(code_of(one(ep, req(1, "tools/list"))) == -32600);
	WB_CHECK(code_of(one(ep, "not json")) == -32700);
	WB_CHECK(code_of(one(ep, R"({"id":1,"method":"ping"})")) == -32600);
	WB_CHECK(ep.era() == AgentEndpoint::Era::Unknown);
}

WB_TEST(claimed_name_kept) {
	AgentEndpoint ep("h", "1");
	one(ep, req(1, "server/discover", "{" + kMeta2026 + "}"));
	std::string other = kMeta2026;
	other.replace(other.find("\"pi\""), 4, "\"other\"");
	one(ep, req(2, "ping", "{" + other + "}"));
	WB_CHECK(ep.claimed_name() == "pi");
	auto notes = ep.take_notes();
	WB_CHECK(notes.size() == 1 && notes[0].find("other") != std::string::npos);
	WB_CHECK(ep.take_notes().empty());
}
