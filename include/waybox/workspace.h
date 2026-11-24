#ifndef _WB_WORKSPACE_H
#define _WB_WORKSPACE_H

/*
 * ext-workspace-v1: tells panels (Waybar's ext/workspaces, ...) about the
 * virtual desktops and lets them switch desktops.
 *
 * All desktops form one workspace group that spans every output. Each
 * desktop is a workspace whose id is its 1-based number and whose name is
 * the configured desktop name; the shown desktop is "active". Panels may
 * activate a workspace; creating, removing and assigning workspaces are not
 * offered.
 */

struct wb_server;

void workspace_init(struct wb_server *server);
/* Tell every bound client about changed desktops (switch, count, names) or
 * outputs. Deferred to an idle callback, so several changes in one
 * dispatch produce one update. */
void workspace_schedule_sync(struct wb_server *server);
/* Drop the global and all listeners; call after clients are destroyed. */
void workspace_finish(struct wb_server *server);

#endif /* _WB_WORKSPACE_H */
