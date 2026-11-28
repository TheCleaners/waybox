#ifndef WB_AGENT_TOOLS_PROVIDER_HPP
#define WB_AGENT_TOOLS_PROVIDER_HPP

/*
 * The compositor's own agent tools (doc/agent-tools.md, "A compositor's own
 * tools"): an MCP server on the socket Waybox hands the hub at launch
 * (AGENT_COMPOSITOR_TOOLS_FD), served on the main loop, where the windows
 * are. Its manifest: data/agents/tools/org.waybox.Waybox.ini.
 *
 * So far the read tools: list_windows, window_titles, list_desktops,
 * list_outputs, get_version. The hub decides who may call them.
 */

struct wb_server;
struct AgentToolsProvider;

/* Serves the tools on `fd` (taken). */
AgentToolsProvider *agent_tools_provider_start(struct wb_server *server, int fd);
/* Stops serving and closes the socket. */
void agent_tools_provider_stop(AgentToolsProvider *p);

/* The manifest's Id, which the compositor names to the hub in Hello. */
inline constexpr const char *kCompositorProviderId = "org.waybox.Waybox";

#endif /* WB_AGENT_TOOLS_PROVIDER_HPP */
