/*
 * The hub's side of an agent's connection, without I/O (mcp_agent.hpp).
 */
#include "mcp_agent.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace wb::agent {

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kUnsupportedVersion = -32022;  /* 2026-07-28 */

/* How long a stateless agent may keep a tool list (the hub's tool lists
 * change with providers and levels: kept short). */
constexpr double kToolsTtlMs = 10000;

const char *kVersionKey = "io.modelcontextprotocol/protocolVersion";
const char *kClientInfoKey = "io.modelcontextprotocol/clientInfo";
const char *kSubscriptionKey = "io.modelcontextprotocol/subscriptionId";

json::Value error_response(const json::Value &id, int code, const std::string &message,
		json::Value data = nullptr) {
	json::Object err{{"code", static_cast<double>(code)}, {"message", message}};
	if (!data.is_null())
		err["data"] = std::move(data);
	return json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(err)}};
}

json::Value result_response(const json::Value &id, json::Object result) {
	return json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json::Array strings(const std::vector<std::string> &v) {
	json::Array a;
	for (const std::string &s : v)
		a.emplace_back(s);
	return a;
}

const json::Value *meta_of(const json::Value *params) {
	return params != nullptr ? params->get("_meta") : nullptr;
}

}  // namespace

Handshake read_handshake(std::string_view line) {
	Handshake hs;
	auto v = json::parse(line);
	if (!v || v->object() == nullptr) {
		hs.error = "the handshake isn't a JSON object";
		return hs;
	}
	const json::Value *ver = v->get("agent-tools");
	if (ver == nullptr || ver->number() == nullptr) {
		hs.error = "no \"agent-tools\" version";
		return hs;
	}
	if (*ver->number() != kHandshakeVersion) {
		hs.error = "unsupported handshake version";
		return hs;
	}
	if (const json::Value *t = v->get("token")) {
		if (t->string() == nullptr) {
			hs.error = "\"token\" isn't a string";
			return hs;
		}
		hs.token = *t->string();
	}
	if (const json::Value *l = v->get("lower")) {
		if (l->object() == nullptr) {
			hs.error = "\"lower\" isn't an object";
			return hs;
		}
		hs.lower = *l->object();
	}
	hs.label = v->str("label");
	hs.ask = v->str("ask");
	hs.ok = true;
	return hs;
}

std::string handshake_answer(const Handshake &hs, const std::string &profile,
		const std::string &hub) {
	json::Object a{{"agent-tools", static_cast<double>(kHandshakeVersion)}, {"ok", hs.ok}};
	if (hs.ok) {
		a["profile"] = profile;
		a["hub"] = hub;
	} else {
		a["error"] = hs.error;
		a["versions"] = json::Array{static_cast<double>(kHandshakeVersion)};
	}
	return json::dump(a);
}

std::string display_name(std::string_view s, size_t max) {
	std::string out;
	size_t shown = 0;
	char esc[16];
	for (size_t i = 0; i < s.size();) {
		if (shown == max) {
			out += "\xe2\x80\xa6";  /* … */
			break;
		}
		unsigned char b = static_cast<unsigned char>(s[i]);
		/* One UTF-8 character, or an escaped byte where it isn't one. */
		size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xe ? 3 : (b >> 3) == 0x1e ? 4 : 0;
		uint32_t cp = 0;
		bool valid = len != 0 && i + len <= s.size();
		if (valid) {
			cp = len == 1 ? b : b & (0xff >> (len + 1));
			for (size_t k = 1; k < len; ++k) {
				unsigned char cb = static_cast<unsigned char>(s[i + k]);
				if ((cb & 0xc0) != 0x80) {
					valid = false;
					break;
				}
				cp = (cp << 6) | (cb & 0x3f);
			}
			/* Overlong forms, surrogates and past Unicode: not characters. */
			static const uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
			if (valid && (cp < least[len] || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff))
				valid = false;
		}
		if (!valid) {
			snprintf(esc, sizeof(esc), "\\x%02x", b);
			out += esc;
			++i;
			++shown;
			continue;
		}
		bool control = cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f);
		bool bidi = cp == 0x061c || cp == 0x200e || cp == 0x200f ||
				(cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
		if (control || bidi) {
			snprintf(esc, sizeof(esc), cp < 0x100 ? "\\x%02x" : "\\u%04x", static_cast<unsigned>(cp));
			out += esc;
		} else {
			out.append(s.substr(i, len));
		}
		i += len;
		++shown;
	}
	return out;
}

const std::vector<std::string> &older_revisions() {
	static const std::vector<std::string> v = {"2025-06-18", "2025-11-25"};
	return v;
}

AgentEndpoint::AgentEndpoint(std::string server_name, std::string server_version,
		size_t max_listens)
	: server_name_(std::move(server_name)), server_version_(std::move(server_version)),
	  max_listens_(max_listens) {}

std::vector<std::string> AgentEndpoint::take_notes() {
	return std::exchange(notes_, {});
}

std::vector<std::string> AgentEndpoint::close_listens() {
	std::vector<std::string> out;
	for (const json::Value &id : listens_) {
		json::Object result = finish(json::Object{});
		json::Object meta = *result["_meta"].object();
		meta[kSubscriptionKey] = id;
		result["_meta"] = std::move(meta);
		out.push_back(json::dump(result_response(id, std::move(result))));
	}
	listens_.clear();
	return out;
}

void AgentEndpoint::claim(const json::Value *client_info) {
	if (client_info == nullptr)
		return;
	std::string name = client_info->str("name");
	if (name.empty())
		return;
	if (claimed_.empty())
		claimed_ = name;
	else if (name != claimed_)
		notes_.push_back("claims another name (\"" + display_name(name) + "\"), kept as \"" +
				display_name(claimed_) + "\"");
}

/* A stateless result carries resultType and the hub's name. */
json::Object AgentEndpoint::finish(json::Object result) const {
	if (era_ == Era::Stateless) {
		result["resultType"] = "complete";
		json::Object meta;
		if (auto it = result.find("_meta"); it != result.end() && it->second.object() != nullptr)
			meta = *it->second.object();
		meta["io.modelcontextprotocol/serverInfo"] =
				json::Object{{"name", server_name_}, {"version", server_version_}};
		result["_meta"] = std::move(meta);
	}
	return result;
}

std::vector<std::string> AgentEndpoint::receive(std::string_view line) {
	std::vector<std::string> out;
	auto msg = json::parse(line);
	if (!msg) {
		out.push_back(json::dump(error_response(nullptr, kParseError, "not JSON")));
		return out;
	}
	if (msg->object() == nullptr || msg->str("jsonrpc") != "2.0") {
		out.push_back(json::dump(error_response(nullptr, kInvalidRequest, "not JSON-RPC 2.0")));
		return out;
	}
	const json::Value *method = msg->get("method");
	const json::Value *id = msg->get("id");
	if (method == nullptr || method->string() == nullptr) {
		/* A response from the agent (the hub sends it no requests yet), or
		 * nonsense: nothing to answer. */
		if (id == nullptr)
			out.push_back(json::dump(error_response(nullptr, kInvalidRequest, "no method")));
		return out;
	}
	if (id == nullptr || !(id->string() != nullptr || id->number() != nullptr)) {
		/* A notification: nothing to answer. A listen stream cancelled by
		 * the agent ends (and, as cancelled, gets no response). */
		if (*method->string() == "notifications/cancelled")
			if (const json::Value *p = msg->get("params"))
				if (const json::Value *rid = p->get("requestId"))
					std::erase(listens_, *rid);
		return out;
	}
	std::vector<std::string> extra;
	json::Value response = request(*method->string(), *id, msg->get("params"), extra);
	for (std::string &e : extra)
		out.push_back(std::move(e));
	if (!response.is_null())
		out.push_back(json::dump(response));
	return out;
}

json::Value AgentEndpoint::request(const std::string &method, const json::Value &id,
		const json::Value *params, std::vector<std::string> &extra) {
	const json::Value *meta = meta_of(params);
	const json::Value *asked = meta != nullptr ? meta->get(kVersionKey) : nullptr;

	/* The first request settles the era. */
	if (era_ == Era::Unknown) {
		if (method == "initialize") {
			era_ = Era::Older;
		} else if (asked != nullptr && asked->string() != nullptr) {
			era_ = Era::Stateless;
		} else {
			return error_response(id, kInvalidRequest,
					"begin with initialize, or name the revision in _meta (" +
					std::string(kVersionKey) + ")");
		}
	}

	if (era_ == Era::Older) {
		if (method == "initialize") {
			if (initialized_)
				return error_response(id, kInvalidRequest, "already initialized");
			std::string want = params != nullptr ? params->str("protocolVersion") : "";
			const auto &known = older_revisions();
			/* A revision is a date; an older one than spoken is refused. A
			 * newer date gets the newest spoken, as MCP's initialize has it:
			 * the agent then goes on or leaves. */
			bool date = want.size() == 10 && want[4] == '-' && want[7] == '-' &&
					std::all_of(want.begin(), want.end(), [](char ch) {
						return ch == '-' || (ch >= '0' && ch <= '9');
					});
			if (!date || want < known.front()) {
				era_ = Era::Unknown;  /* (it may try again) */
				return error_response(id, kInvalidParams, "unsupported protocol version",
						json::Object{{"supported", strings(known)}, {"requested", want}});
			}
			/* A known one, or the newest older one for anything newer. */
			revision_ = std::find(known.begin(), known.end(), want) != known.end()
					? want : known.back();
			initialized_ = true;
			claim(params->get("clientInfo"));
			return result_response(id, json::Object{
				{"protocolVersion", revision_},
				{"capabilities", json::Object{{"tools", json::Object{{"listChanged", true}}}}},
				{"serverInfo", json::Object{{"name", server_name_}, {"version", server_version_}}},
			});
		}
		if (method == "ping")
			return result_response(id, json::Object{});
		if (!initialized_)
			return error_response(id, kInvalidRequest, "not initialized");
		if (method == "tools/list")
			return result_response(id, json::Object{{"tools", json::Array{}}});
		if (method == "tools/call") {
			std::string name = params != nullptr ? params->str("name") : "";
			return error_response(id, kInvalidParams, "unknown tool: " + name);
		}
		return error_response(id, kMethodNotFound, "not supported: " + method);
	}

	/* Stateless: each request names its revision. */
	if (asked == nullptr || asked->string() == nullptr)
		return error_response(id, kInvalidParams,
				std::string("no revision in _meta (") + kVersionKey + ")");
	if (*asked->string() != kStatelessRevision)
		return error_response(id, kUnsupportedVersion, "unsupported protocol version",
				json::Object{{"supported", json::Array{kStatelessRevision}},
						{"requested", *asked->string()}});
	revision_ = kStatelessRevision;
	claim(meta->get(kClientInfoKey));
	if (method == "server/discover")
		return result_response(id, finish(json::Object{
			{"supportedVersions", json::Array{kStatelessRevision}},
			{"capabilities", json::Object{{"tools", json::Object{{"listChanged", true}}}}},
			{"ttlMs", kToolsTtlMs},
			{"cacheScope", "private"},
		}));
	if (method == "ping")
		return result_response(id, finish(json::Object{}));
	if (method == "tools/list")
		return result_response(id, finish(json::Object{
			{"tools", json::Array{}},
			{"ttlMs", kToolsTtlMs},
			{"cacheScope", "private"},
		}));
	if (method == "tools/call") {
		std::string name = params != nullptr ? params->str("name") : "";
		return error_response(id, kInvalidParams, "unknown tool: " + name);
	}
	if (method == "subscriptions/listen") {
		/* Acknowledged with what's honored (tool-list changes only); the
		 * stream stays open: its response comes only when it ends. */
		const json::Value *wanted = params != nullptr ? params->get("notifications") : nullptr;
		json::Object honored;
		if (wanted != nullptr)
			if (const json::Value *t = wanted->get("toolsListChanged"))
				if (t->boolean() != nullptr && *t->boolean())
					honored["toolsListChanged"] = true;
		if (std::find(listens_.begin(), listens_.end(), id) != listens_.end())
			return error_response(id, kInvalidRequest, "a listen stream with this id is open");
		if (listens_.size() >= max_listens_)
			return error_response(id, kInvalidRequest, "too many open listen streams (MaxListens)");
		listens_.push_back(id);
		extra.push_back(json::dump(json::Object{
			{"jsonrpc", "2.0"},
			{"method", "notifications/subscriptions/acknowledged"},
			{"params", json::Object{
				{"_meta", json::Object{{kSubscriptionKey, id}}},
				{"notifications", std::move(honored)},
			}},
		}));
		return nullptr;
	}
	return error_response(id, kMethodNotFound, "not supported: " + method);
}

}  // namespace wb::agent
