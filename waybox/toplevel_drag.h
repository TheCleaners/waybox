#ifndef WB_TOPLEVEL_DRAG_H
#define WB_TOPLEVEL_DRAG_H

#include "waybox/server.h"

/*
 * xdg-toplevel-drag-v1: a window carried along by a drag-and-drop (a
 * browser tab torn off into a window of its own).
 */
void init_toplevel_drag(struct wb_server *server);

/* The window the drag in progress carries, if any. */
struct wb_toplevel *toplevel_drag_window(struct wb_server *server);

/* On pointer motion during a drag: the carried window follows. */
void toplevel_drag_motion(struct wb_server *server);

/* On map: a window attached to the drag in progress opens at the pointer
 * (true), instead of where the placement policy would put it. */
bool toplevel_drag_place(struct wb_toplevel *toplevel);

/* On unmap: a window is detached from its drag. */
void toplevel_drag_unmapped(struct wb_toplevel *toplevel);

#endif
