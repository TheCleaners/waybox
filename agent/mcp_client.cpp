/*
 * The hub's side of a provider's connection (mcp_client.hpp).
 */
#include "mcp_client.hpp"

#include <utility>

namespace wb::agent {

namespace {

constexpr const char *kStateless = "2026-07-28";
constexpr const char *kOlder = "2025-11-25";
constexpr int kUnsupportedVersion = -32022;

/* Request ids the client uses for itself (calls are numbered "c…"). */
constexpr const char *kDiscover = "discover";
constexpr const char *kInitialize = "initialize";
constexpr const char *kListen = "listen";

}  // namespace

McpClient::McpClient(std::string name, std::string version)
	: name_(std::move(name)), version_(std::move(version)) {}

std::string McpClient::id() {
	return "c" + std::to_string(next_++);
}

json::Object McpClient::meta() const {
	return json::Object{
		{"io.modelcontextprotocol/protocolVersion", kStateless},
		{"io.modelcontextprotocol/clientCapabilities", json::Object{}},
		{"io.modelcontextprotocol/clientInfo",
				json::Object{{"name", name_}, {"version", version_}}},
	};
}

std::string McpClient::request(const std::string &rid, const std::string &method,
		json::Object params) {
	if (era_ == Era::Stateless || method == "server/discover")
		params["_meta"] = meta();
	return json::dump(json::Object{{"jsonrpc", "2.0"}, {"id", rid}, {"method", method},
			{"params", std::move(params)}});
}

std::vector<std::string> McpClient::start() {
	return {request(kDiscover, "server/discover", {})};
}

std::vector<std::string> McpClient::timed_out() {
	if (state_ != State::Discovering)
		return {};
	era_ = Era::Older;
	state_ = State::Initializing;
	return {request(kInitialize, "initialize", json::Object{
		{"protocolVersion", kOlder},
		{"capabilities", json::Object{}},
		{"clientInfo", json::Object{{"name", name_}, {"version", version_}}},
	})};
}

/* tools/list, page by page; a change meanwhile lists again after. */
std::vector<std::string> McpClient::list(const std::string &cursor) {
	if (listing_now_ && cursor.empty()) {
		relist_ = true;
		return {};
	}
	listing_now_ = true;
	if (cursor.empty())
		listing_.clear();
	json::Object params;
	if (!cursor.empty())
		params["cursor"] = cursor;
	return {request("list:" + std::to_string(next_++), "tools/list", std::move(params))};
}

bool McpClient::take_tools_changed() {
	return std::exchange(tools_changed_, false);
}

std::vector<std::string> McpClient::call(const std::string &tool, const json::Object &args,
		Done done, Progress progress) {
	if (state_ != State::Ready) {
		done(false, json::Object{{"code", -32603.0}, {"message", "the provider isn't ready"},
				{"not_sent", true}});
		return {};
	}
	last_call_ = id();
	calls_[last_call_] = std::move(done);
	if (progress)
		progress_[last_call_] = std::move(progress);
	std::string line = request(last_call_, "tools/call",
			json::Object{{"name", tool}, {"arguments", args}});
	/* Its progress asked for, by its own id (both eras take _meta). */
	auto v = json::parse(line);
	json::Object msg = *v->object();
	json::Object params = *msg["params"].object();
	json::Object meta = params.count("_meta") ? *params["_meta"].object() : json::Object{};
	meta["progressToken"] = last_call_;
	params["_meta"] = std::move(meta);
	msg["params"] = std::move(params);
	return {json::dump(msg)};
}

std::vector<std::string> McpClient::cancel(const std::string &tool_call) {
	if (calls_.count(tool_call) == 0)
		return {};
	json::Object params{{"requestId", tool_call}};
	if (era_ == Era::Stateless)
		params["_meta"] = meta();
	return {json::dump(json::Object{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
			{"params", std::move(params)}})};
}

void McpClient::lost(const std::string &why) {
	state_ = State::Failed;
	if (error_.empty())
		error_ = why;
	auto calls = std::exchange(calls_, {});
	progress_.clear();
	for (auto &[rid, done] : calls)
		done(false, json::Object{{"code", -32603.0}, {"message", why}, {"lost", true}});
}

std::vector<std::string> McpClient::receive(std::string_view line) {
	auto msg = json::parse(line);
	if (!msg || msg->object() == nullptr)
		return {};
	const json::Value *method = msg->get("method");
	const json::Value *rid = msg->get("id");
	if (method != nullptr && method->string() != nullptr) {
		/* From the provider: tool-list changes; requests of its own aren't
		 * taken (sampling and elicitation come later). */
		if (*method->string() == "notifications/tools/list_changed" && state_ == State::Ready)
			return list();
		if (*method->string() == "notifications/progress") {
			const json::Value *p = msg->get("params");
			const json::Value *tok = p != nullptr ? p->get("progressToken") : nullptr;
			const json::Value *n = p != nullptr ? p->get("progress") : nullptr;
			if (tok != nullptr && tok->string() != nullptr && n != nullptr && n->number() != nullptr) {
				auto it = progress_.find(*tok->string());
				if (it != progress_.end()) {
					const json::Value *total = p->get("total");
					it->second(*n->number(), total != nullptr ? *total : json::Value(),
							p->str("message"));
				}
			}
			return {};
		}
		if (rid != nullptr)
			return {json::dump(json::Object{{"jsonrpc", "2.0"}, {"id", *rid},
					{"error", json::Object{{"code", -32601.0}, {"message", "not taken"}}}})};
		return {};
	}
	if (rid == nullptr || rid->string() == nullptr)
		return {};
	return on_response(*rid->string(), *msg);
}

std::vector<std::string> McpClient::on_response(const std::string &rid, const json::Value &msg) {
	const json::Value *result = msg.get("result");
	const json::Value *error = msg.get("error");
	if (rid == kDiscover) {
		if (state_ != State::Discovering)
			return {};
		if (result != nullptr) {
			const json::Value *sup = result->get("supportedVersions");
			bool ours = false;
			if (sup != nullptr && sup->array() != nullptr)
				for (const json::Value &v : *sup->array())
					ours |= v.string() != nullptr && *v.string() == kStateless;
			if (!ours)
				return timed_out();  /* stateless, but not a revision spoken here */
			era_ = Era::Stateless;
			state_ = State::Ready;
			std::vector<std::string> out = {request(kListen, "subscriptions/listen",
					json::Object{{"notifications", json::Object{{"toolsListChanged", true}}}})};
			for (std::string &l : list())
				out.push_back(std::move(l));
			state_ = State::Initializing;  /* (until the first list) */
			return out;
		}
		const json::Value *code = error != nullptr ? error->get("code") : nullptr;
		if (code != nullptr && code->number() != nullptr && *code->number() == kUnsupportedVersion) {
			state_ = State::Failed;
			error_ = "the provider speaks no revision the hub does";
			return {};
		}
		return timed_out();
	}
	if (rid == kInitialize) {
		if (result == nullptr) {
			state_ = State::Failed;
			error_ = "initialize refused";
			return {};
		}
		std::vector<std::string> out = {json::dump(json::Object{
				{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})};
		for (std::string &l : list())
			out.push_back(std::move(l));
		return out;
	}
	if (rid == kListen)
		return {};  /* the stream ended */
	if (rid.rfind("list:", 0) == 0) {
		if (result == nullptr) {
			listing_now_ = false;
			state_ = State::Failed;
			error_ = "tools/list refused";
			return {};
		}
		if (const json::Value *t = result->get("tools"); t != nullptr && t->array() != nullptr)
			for (const json::Value &tool : *t->array())
				listing_.push_back(tool);
		std::string cursor = result->str("nextCursor");
		if (!cursor.empty()) {
			listing_now_ = false;
			return list(cursor);
		}
		listing_now_ = false;
		tools_ = std::move(listing_);
		listing_.clear();
		state_ = State::Ready;
		tools_changed_ = true;
		if (std::exchange(relist_, false))
			return list();
		return {};
	}
	auto it = calls_.find(rid);
	if (it == calls_.end())
		return {};
	Done done = std::move(it->second);
	calls_.erase(it);
	progress_.erase(rid);
	if (result != nullptr && result->object() != nullptr)
		done(true, *result->object());
	else if (error != nullptr && error->object() != nullptr)
		done(false, *error->object());
	else
		done(false, json::Object{{"code", -32603.0}, {"message", "a malformed answer"}});
	return {};
}

}  // namespace wb::agent
