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
 * steps that bring them. The hub is the program WAYBOX_AGENT_HUB names
 * (until waybox-agent-hub and its supervision exist).
 */

struct wb_server;

/* Starts the hub, if there's one to start. */
void agent_compositor_init(struct wb_server *server);
/* Ends the D-Bus thread; the hub sees its socket close. */
void agent_compositor_finish(struct wb_server *server);
/* The lock, away or stopped state may have changed: the hub hears of what
 * did (PropertiesChanged). */
void agent_compositor_state_changed(struct wb_server *server);
/* SIGCHLD: the hub, if it exited. */
void agent_compositor_reap(struct wb_server *server);

#endif /* WB_AGENT_COMPOSITOR_HPP */
