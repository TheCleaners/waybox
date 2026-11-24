#ifndef _WB_WINDOW_ICON_H
#define _WB_WINDOW_ICON_H

struct wb_server;
struct wb_toplevel;

/*
 * Window icons for the titlebar (<titleLayout> N): the icon the client sets
 * (xdg-toplevel-icon: pixels or an icon name), else its desktop file's
 * Icon=, else its app ID as an icon name.
 */
void init_window_icons(struct wb_server *server);

/* Pick the window's icon again (app ID changed, frame created, ...) and
 * give it to its frame. */
void toplevel_update_icon(struct wb_toplevel *toplevel);

/* Free the window's icons (before it is freed). */
void toplevel_free_icon(struct wb_toplevel *toplevel);

#endif /* _WB_WINDOW_ICON_H */
