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

class AgentEndpoint {
public:
	enum class Era { Unknown, Older, Stateless };

	AgentEndpoint(std::string server_name, std::string server_version,
			size_t max_listens = kMaxListens);

	/* A line from the agent (no newline); the lines to send back. */
	std::vector<std::string> receive(std::string_view line);

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
	Era era_ = Era::Unknown;
	std::string revision_, claimed_;
	bool initialized_ = false;
	std::vector<json::Value> listens_;  /* open subscriptions/listen ids */
	std::vector<std::string> notes_;

	void claim(const json::Value *client_info);
	json::Value request(const std::string &method, const json::Value &id,
			const json::Value *params, std::vector<std::string> &extra);
	json::Object finish(json::Object result) const;
};

}  // namespace wb::agent

#endif /* WB_AGENT_MCP_AGENT_HPP */
