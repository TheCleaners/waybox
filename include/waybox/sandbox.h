#ifndef _WB_SANDBOX_H
#define _WB_SANDBOX_H

struct wb_server;

/* security-context-v1 and the global filter: sandboxed clients don't see
 * privileged protocols (see security.hpp), and only Xwayland sees
 * xwayland_shell_v1. */
void init_sandbox(struct wb_server *server);

#endif /* _WB_SANDBOX_H */
