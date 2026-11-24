#include "waybox/server.h"

bool create_idle_manager(struct wb_server *server);
/* Input happened: tell idle clients (swayidle), note it for <idle
 * screenOff>, and switch back on the screens that switched off. */
void idle_activity(struct wb_server *server, struct wlr_seat *seat);
/* <idle> may have changed: count the timeout from now. */
void idle_reconfigured(struct wb_server *server);
void finish_idle(struct wb_server *server);
