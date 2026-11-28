#ifndef WB_AGENT_AGENTS_HPP
#define WB_AGENT_AGENTS_HPP

/*
 * The hub's agent socket ($XDG_RUNTIME_DIR/agents/tools/agent): shims
 * connect, send the handshake, then speak MCP (mcp_agent.hpp). On the
 * hub's GLib main loop. For now every agent is at "safe": bindings and
 * their tokens come with the settings (doc/agent-tools.md, step 3).
 *
 * Per connection: the agent's credentials from the socket (only this
 * user's), the handshake within HandshakeWait, messages of at most
 * MaxFrame; the agent done sending (its side half-closed), the hub answers
 * what's left, then closes.
 */
#include <string>

#include "tunables.hpp"

namespace wb::agent {

/* Listens at `path` (a stale socket there is replaced), with the hub's
 * tunables (MaxFrame, MaxQueued, MaxAgents, MaxListens, HandshakeWait). */
bool agents_listen(const std::string &path, const std::string &hub_version,
		const Tunables &t);
/* Closes every agent's connection and the socket. */
void agents_stop();

}  // namespace wb::agent

#endif /* WB_AGENT_AGENTS_HPP */
