#ifndef WB_ATOMIC_FILE_HPP
#define WB_ATOMIC_FILE_HPP

#include <string>

namespace wb {

/*
 * `text` written to `path` whole or not at all: into a temporary file
 * beside it, made new (a unique name, created exclusively: an entry
 * already there, a symlink to another file say, is never written through
 * or shared with another save), written in full and synced, its mode set
 * through it (the file's own, if there was one; else the usual for a new
 * file, 0666 less the umask), then renamed over `path`. On failure only
 * that temporary file is removed, `path` left as it was, and `*error` says
 * why. False on failure.
 */
bool write_file_atomic(const std::string &path, const std::string &text, std::string *error);

}  // namespace wb

#endif /* WB_ATOMIC_FILE_HPP */
