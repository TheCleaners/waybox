#ifndef WB_AGENT_COMPOSITOR_HPP
#define WB_AGENT_COMPOSITOR_HPP

/*
 * The compositor's side of org.freedesktop.AgentCompositor1, the interface
 * between the agent hub and the compositor that starts it
 * (doc/agent-tools.md, doc/dbus/org.freedesktop.AgentCompositor1.xml).
 *
 * Peer-to-peer D-Bus on a socket Waybox hands the hub at launch (fd 3,
 * named by AGENT_COMPOSITOR_FD): no bus daemon, no bus name. Served on the
 * shared GLib thread (glib_thread.hpp), never one of its own: calls that
 * need the compositor go through the main thread's queue; properties are
 * answered from a snapshot the main thread keeps current, so the GLib
 * thread never waits on the main one.
 *
 * So far: Hello, and the Locked, Away and AssistantsStopped properties
 * with their changes; the other members answer "not supported" until the
 * steps that bring them.
 *
 * The hub (waybox-agent-hub; WAYBOX_AGENT_HUB names a stand-in) runs while
 * agent tools are on (Enabled=true in agents/tools/settings.ini), and is
 * supervised: restarted after a growing delay when it exits, not asked
 * to, until it has crashed HubRestarts times within a minute; then not
 * again until agent tools are reconfigured. The state it reads (the
 * stopped state above all) is the compositor's, kept through restarts.
 */

struct wb_server;

/* Starts the hub, if agent tools are on. */
void agent_compositor_init(struct wb_server *server);
/* Ends the hub (its connection first: it sees its socket close). */
void agent_compositor_finish(struct wb_server *server);
/* Settings may have changed: agent tools turned on or off; a hub given up
 * on gets another go. */
void agent_compositor_reconfigure(struct wb_server *server);
/* The lock, away or stopped state may have changed: the hub hears of what
 * did (PropertiesChanged). */
void agent_compositor_state_changed(struct wb_server *server);
/* SIGCHLD: the hub, if it exited. */
void agent_compositor_reap(struct wb_server *server);

#endif /* WB_AGENT_COMPOSITOR_HPP */
