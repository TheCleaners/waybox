#ifndef WB_TRUST_HPP
#define WB_TRUST_HPP

#include <string>
#include <sys/types.h>

namespace wb {

/*
 * Whether only the system could have changed `path` (a package's files):
 * it and every directory above it, as spelled and with symlinks resolved,
 * owned by `owner` (root) and writable by no group or others; a directory
 * others may write to counts only if it's sticky (no one can replace
 * another's entry in it). Anything that can't be checked (missing,
 * unreadable) isn't. The basis for running a settings page without asking.
 */
bool owned_by_system(const std::string &path, uid_t owner = 0);

}  // namespace wb

#endif /* WB_TRUST_HPP */
