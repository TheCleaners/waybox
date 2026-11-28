#ifndef WB_AGENT_MCP_AGENT_HPP
#define WB_AGENT_MCP_AGENT_HPP

/*
 * The hub's side of an agent's connection (doc/agent-tools.md: "Transport",
 * "MCP revisions"), without I/O: the shim's handshake line read and
 * answered, then an MCP endpoint that takes the agent's lines and gives the
 * lines to send back, in either era:
 * - 2025-06-18 and 2025-11-25, which begin with `initialize`;
 * - 2026-07-28, stateless: each request names its revision in `_meta`.
 * The first request settles the era for the connection.
 *
 * No tools yet: the compositor's come with the next step (tools/list is
 * empty, a call names an unknown tool). Pure; unit-tested.
 */
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/json.hpp"

namespace wb::agent {

/* The handshake's own version ("agent-tools": 1). */
inline constexpr int kHandshakeVersion = 1;

/* The shim's first line, read. */
struct Handshake {
	bool ok = false;
	std::string error;          /* why not, for the answer */
	std::optional<std::string> token;  /* (never logged) */
	json::Object lower;         /* narrowing asked for, as sent */
	std::string label;          /* the user's name for this agent */
	std::string ask;            /* a one-time ask ticket */
};
Handshake read_handshake(std::string_view line);
/* The hub's one-line answer (no newline). */
std::string handshake_answer(const Handshake &hs, const std::string &profile,
		const std::string &hub);

/* What an agent says of itself (a label, a claimed name), for a log line or
 * the screen: control characters, bidirectional overrides and broken UTF-8
 * escaped, at most `max` characters ("…" past that). Agents choose these
 * strings: shown raw, they could forge log lines or reorder what's read. */
std::string display_name(std::string_view s, size_t max = 64);

/* Open subscriptions/listen streams an agent may hold (MaxListens). */
inline constexpr size_t kMaxListens = 8;

/* The revisions spoken (oldest first), and the stateless one. */
const std::vector<std::string> &older_revisions();
inline constexpr const char *kStatelessRevision = "2026-07-28";

class McpServer;

/* Where an McpServer's tools come from: the hub's providers, or a
 * provider's own tools. */
class ToolSource {
public:
	virtual ~ToolSource() = default;
	/* The tool definitions, as clients get them. */
	virtual json::Array list_tools() = 0;
	/* Calls a tool; the source answers with server.complete() or
	 * server.fail(), now or later. */
	virtual void call(McpServer &server, const json::Value &id, const std::string &name,
			const json::Object &args) = 0;
	/* The client cancelled a call still under way (no answer is sent for
	 * it from now). */
	virtual void cancel(const json::Value &id) { (void)id; }
};

/* An MCP server endpoint, either era: lines in, lines out. */
class McpServer {
public:
	enum class Era { Unknown, Older, Stateless };

	McpServer(std::string server_name, std::string server_version,
			size_t max_listens = kMaxListens, ToolSource *tools = nullptr,
			std::string instructions = {});

	/* A line from the client (no newline); the lines to send back (answers
	 * a tool source gave meanwhile among them). */
	std::vector<std::string> receive(std::string_view line);

	/* A call's answer: a tool result (content, structuredContent, isError),
	 * or an error. Nothing is sent for a call not under way (cancelled,
	 * answered already). The lines go out with take_output(). */
	void complete(const json::Value &id, json::Object result);
	void fail(const json::Value &id, int code, const std::string &message);
	/* Lines answered since the last receive() or take_output(). */
	std::vector<std::string> take_output();
	/* The tool list changed: the client is told (older era: a
	 * notification; stateless: on listen streams that asked). */
	void tools_changed();
	size_t calls_under_way() const { return calls_.size(); }

	Era era() const { return era_; }
	/* The revision in use (the older era: as negotiated). */
	const std::string &revision() const { return revision_; }
	/* The name the agent first claimed (clientInfo), if any. */
	const std::string &claimed_name() const { return claimed_; }
	/* Notes for the hub's log since the last call (a second name claimed). */
	std::vector<std::string> take_notes();
	/* The connection ends gracefully: each open listen stream's closing
	 * result (2026-07-28), to send before closing. */
	std::vector<std::string> close_listens();
	size_t open_listens() const { return listens_.size(); }

private:
	std::string server_name_, server_version_;
	size_t max_listens_;
	ToolSource *tools_;
	std::string instructions_;
	std::vector<json::Value> calls_;        /* calls under way, by id */
	std::vector<json::Value> tool_listens_; /* listens that asked for tool changes */
	std::vector<std::string> outbox_;
	Era era_ = Era::Unknown;
	std::string revision_, claimed_;
	bool initialized_ = false;
	std::vector<json::Value> listens_;  /* open subscriptions/listen ids */
	std::vector<std::string> notes_;

	void claim(const json::Value *client_info);
	json::Value begin_call(const json::Value &id, const json::Value *params);
	json::Value request(const std::string &method, const json::Value &id,
			const json::Value *params, std::vector<std::string> &extra);
	json::Object finish(json::Object result) const;
};

}  // namespace wb::agent

#endif /* WB_AGENT_MCP_AGENT_HPP */
