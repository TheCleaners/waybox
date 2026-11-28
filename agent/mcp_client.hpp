#ifndef WB_AGENT_MCP_CLIENT_HPP
#define WB_AGENT_MCP_CLIENT_HPP

/*
 * The hub's side of a provider's connection (doc/agent-tools.md, "MCP
 * revisions"), without I/O: an MCP client in either era. It begins with
 * server/discover: an answer, or a modern error, means 2026-07-28; another
 * error, or none within a moment (timed_out), means the older era
 * (initialize). Then tools/list (every page), kept current: the older
 * era's list_changed notification, or a listen stream for it. Calls go out
 * with call() and come back to their callback. Pure; unit-tested.
 */
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/json.hpp"

namespace wb::agent {

class McpClient {
public:
	enum class State { Discovering, Initializing, Ready, Failed };
	enum class Era { Unknown, Older, Stateless };
	/* A call's end: ok and the result, or not and the error object. */
	using Done = std::function<void(bool ok, const json::Object &body)>;

	McpClient(std::string name, std::string version);

	/* The first lines to send. */
	std::vector<std::string> start();
	/* A line from the provider; lines to send back. */
	std::vector<std::string> receive(std::string_view line);
	/* No answer to server/discover in time: the older era is tried. */
	std::vector<std::string> timed_out();
	/* A tool called; `done` once it's answered (or the connection ends). */
	std::vector<std::string> call(const std::string &tool, const json::Object &args, Done done);
	/* A call no longer wanted: the provider is told; `done` isn't called. */
	std::vector<std::string> cancel(const std::string &tool_call);
	/* The connection ended: every call still out ends, as failed. */
	void lost(const std::string &why);

	State state() const { return state_; }
	Era era() const { return era_; }
	/* The provider's tools, as it lists them (complete once Ready). */
	const json::Array &tools() const { return tools_; }
	/* True once each time the tool list (re)arrived. */
	bool take_tools_changed();
	/* Why it failed, if it did. */
	const std::string &error() const { return error_; }
	/* The id of the last call() (for cancel()). */
	const std::string &last_call() const { return last_call_; }

private:
	std::string name_, version_;
	State state_ = State::Discovering;
	Era era_ = Era::Unknown;
	json::Array tools_, listing_;
	bool listing_now_ = false, relist_ = false, tools_changed_ = false;
	std::string error_, last_call_;
	unsigned next_ = 1;
	std::map<std::string, Done> calls_;  /* by request id */

	std::string id();
	json::Object meta() const;
	std::string request(const std::string &id, const std::string &method, json::Object params);
	std::vector<std::string> list(const std::string &cursor = {});
	std::vector<std::string> on_response(const std::string &id, const json::Value &msg);
};

}  // namespace wb::agent

#endif /* WB_AGENT_MCP_CLIENT_HPP */
