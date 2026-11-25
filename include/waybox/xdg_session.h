#ifndef WB_XDG_SESSION_H
#define WB_XDG_SESSION_H

#include <vector>

#include "waybox/session_store.hpp"

struct wb_server;
struct wb_toplevel;

/*
 * Session restore (xdg-session-management-v1, xdg_session.cpp): apps that
 * ask get their windows back where they were. The state is kept in
 * $XDG_STATE_HOME/waybox/sessions.json; <waybox><sessionRestore
 * enabled="no"/> turns it off (apps then get sessions that keep nothing).
 */
void init_xdg_session(struct wb_server *server);
/* Saves; before the display goes. */
void finish_xdg_session(struct wb_server *server);

/* A window in a session moved, resized, changed state or desktop: saved
 * a moment later (one save for a burst of changes). */
void xdg_session_toplevel_changed(struct wb_toplevel *toplevel);
/* Its state now, into the store (not yet saved). */
void xdg_session_snapshot(struct wb_toplevel *toplevel);
void xdg_session_toplevel_unmapped(struct wb_toplevel *toplevel);
void xdg_session_toplevel_destroyed(struct wb_toplevel *toplevel);
/* At its first commit: tells the app it's being restored, before the
 * configure that gives it its old size. */
void xdg_session_initial_configure(struct wb_toplevel *toplevel);

/* The outputs now, to put a restored window back on. */
std::vector<wb::OutputBox> xdg_session_outputs(struct wb_server *server);

#endif
