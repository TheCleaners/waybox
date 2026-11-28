#ifndef WB_AGENT_PROVIDERS_HPP
#define WB_AGENT_PROVIDERS_HPP

/*
 * The hub's providers (doc/agent-tools.md, "Providers"), on its GLib main
 * loop. So far the compositor's own: its tools on the socket the
 * compositor hands the hub (AGENT_COMPOSITOR_TOOLS_FD), its manifest the
 * one Hello names, read from the XDG data folders (agents/tools/). Other
 * providers (manifests, activation, registration, approval) come later.
 */
#include <functional>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "mcp_client.hpp"
#include "tunables.hpp"

namespace wb::agent {

/* The compositor's provider: its tools on `fd` (taken), its manifest `id`. */
void providers_start_compositor(int fd, const std::string &id, const std::string &hub_version,
		const Tunables &t);
void providers_stop();

/* What's offered now (any level: agents filter by theirs). */
const std::vector<OfferedTool> &offered_tools();
/* Told whenever that changes. */
void providers_on_change(std::function<void()> f);

/* Calls an offered tool; `done` on the main loop. A handle for
 * providers_cancel (0: answered already, as failed). */
unsigned providers_call(const OfferedTool &t, const json::Object &args, McpClient::Done done);
void providers_cancel(unsigned handle);

}  // namespace wb::agent

#endif /* WB_AGENT_PROVIDERS_HPP */
