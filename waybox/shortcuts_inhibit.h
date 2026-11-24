#ifndef WB_SHORTCUTS_INHIBIT_H
#define WB_SHORTCUTS_INHIBIT_H

#include "waybox/server.h"

/* keyboard-shortcuts-inhibit-v1: a focused VM viewer or remote desktop can
 * take the keys Waybox would otherwise handle (Alt+Tab, Super+...). Its
 * inhibitor is active only while it has the keyboard; Super+Escape takes
 * the shortcuts back (and again gives them to it). VT switching always
 * works. <waybox><shortcutsInhibit allow="no"/> turns it off. */
void init_shortcuts_inhibit(struct wb_server *server);
void finish_shortcuts_inhibit(struct wb_server *server);
/* Whether the focused surface has the shortcuts now. */
bool shortcuts_inhibited(struct wb_server *server);
/* Super+Escape: take the shortcuts back from the focused surface, or give
 * them to it again. False if it has no inhibitor. */
bool shortcuts_inhibit_toggle(struct wb_server *server);
/* Keyboard focus or the settings changed: (de)activate inhibitors. */
void shortcuts_inhibit_update(struct wb_server *server);

#endif
