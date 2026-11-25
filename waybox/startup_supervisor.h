#ifndef WB_STARTUP_SUPERVISOR_H
#define WB_STARTUP_SUPERVISOR_H

struct wb_server;

/* Start, stop and restart rc.xml's <startup> programs to match the current
 * config: at startup and on each Reconfigure. Without a <startup> list it
 * starts nothing. */
void startup_apply(struct wb_server *server);
/* The input method's variables (<inputMethod><env>), set for everything
 * started from now on; before the autostart script runs. */
void startup_apply_env(struct wb_server *server);
/* On exit: the D-Bus/systemd activation environment gets back what it had
 * before Waybox set the input method's variables. */
void startup_restore_env(struct wb_server *server);
/* SIGCHLD: reap the programs that exited (and schedule restarts). */
void startup_reap(struct wb_server *server);
/* Waybox is exiting: end every program it started. */
void startup_finish(struct wb_server *server);

#endif
