#ifndef _WB_SESSION_LOCK_H
#define _WB_SESSION_LOCK_H

/*
 * ext-session-lock-v1: lets a locker such as swaylock lock the session.
 *
 * While locked, a scene tree above everything else (panels, menus, OSDs)
 * covers every output with an opaque rectangle and holds the locker's
 * surfaces. Keyboard focus can only go to a lock surface, pointer input only
 * reaches lock surfaces, and compositor key and mouse bindings are disabled
 * (VT switching still works). If the locker dies without unlocking, the
 * session stays locked (blank) until a new locker connects and unlocks it.
 */

#include "waybox/wlroots.hpp"

struct wb_server;
struct wb_output;

void init_session_lock(struct wb_server *server);
void finish_session_lock(struct wb_server *server);

/* True from the moment a locker locks until it unlocks. */
bool session_locked(const struct wb_server *server);
/* An output is going away or off: stop waiting for its locked frame. */
void session_lock_output_gone(struct wb_server *server, struct wlr_output *output);

/* True if `surface` is a lock surface of the current lock. */
bool session_lock_owns_surface(const struct wb_server *server,
		struct wlr_surface *surface);

/* The lock surface (and surface-local coordinates) at layout point (lx, ly),
 * or null. */
struct wlr_surface *session_lock_surface_at(struct wb_server *server,
		double lx, double ly, double *sx, double *sy);

/* Give keyboard focus to the lock surface on the output under the cursor (or
 * any lock surface). */
void session_lock_focus(struct wb_server *server);

/* Outputs were added, removed or reconfigured: resize the blanking rects and
 * reconfigure the lock surfaces. */
void session_lock_arrange(struct wb_server *server);

/* Re-raise the lock tree after something else raised a tree to the top. */
void session_lock_keep_on_top(struct wb_server *server);

/* An output committed a frame; used to delay "locked" until every output has
 * actually shown the blanked scene, as the protocol requires. */
void session_lock_output_frame(struct wb_output *output);

#endif /* _WB_SESSION_LOCK_H */
