#ifndef WB_ATOMIC_FILE_HPP
#define WB_ATOMIC_FILE_HPP

#include <optional>
#include <sys/types.h>
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
bool write_file_atomic(const std::string &path, const std::string &text, std::string *error,
		std::optional<mode_t> mode = std::nullopt);

/* A regular file's contents, at most `max_bytes` (a longer one isn't
 * read at all); nullopt for anything else (missing, a FIFO or device,
 * which is opened without blocking and refused). For files a client's
 * choice of name picks, read on the compositor's thread. */
std::optional<std::string> read_regular_file(const std::string &path, size_t max_bytes);

}  // namespace wb

#endif /* WB_ATOMIC_FILE_HPP */
