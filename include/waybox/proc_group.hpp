#pragma once

#include <sys/types.h>

namespace wb {

/* Whether a process group has a running member other than `except` (its
 * leader, usually: exited, kept unreaped so that the group's id can't be
 * reused, which a zombie still in the group would answer kill(-pgid, 0)
 * for). Zombies don't count. /proc on Linux, sysctl on the BSDs. */
bool group_has_others(pid_t pgid, pid_t except);

}  // namespace wb
