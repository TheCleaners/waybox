#include "waybox/atomic_file.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace wb {

namespace {

bool write_all(int fd, const std::string &text) {
	size_t done = 0;
	while (done < text.size()) {
		ssize_t n = write(fd, text.data() + done, text.size() - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		done += static_cast<size_t>(n);
	}
	return true;
}

/* 0666 less the umask: what a file made the ordinary way gets. */
mode_t new_file_mode() {
	mode_t mask = umask(0);
	umask(mask);
	return 0666 & ~mask;
}

}  // namespace

bool write_file_atomic(const std::string &path, const std::string &text, std::string *error,
		std::optional<mode_t> fixed_mode) {
	auto fail = [&](const std::string &why) {
		if (error != nullptr)
			*error = path + ": " + why;
		return false;
	};
	/* The mode given, else the one to keep (the file's own; a script stays
	 * executable). */
	struct stat st;
	mode_t mode = fixed_mode ? *fixed_mode
			: stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : new_file_mode();

	std::string name = path + ".wbconf-XXXXXX";
	std::vector<char> tmp(name.begin(), name.end());
	tmp.push_back('\0');
	int fd = mkstemp(tmp.data());  /* new, exclusive, 0600; no link followed */
	if (fd < 0)
		return fail(strerror(errno));
	bool ok = write_all(fd, text) && fchmod(fd, mode) == 0 && fsync(fd) == 0;
	int saved = errno;
	if (close(fd) != 0 && ok) {
		ok = false;
		saved = errno;
	}
	if (!ok || rename(tmp.data(), path.c_str()) != 0) {
		if (ok)
			saved = errno;
		unlink(tmp.data());  /* only the one this save made */
		return fail(strerror(saved));
	}
	return true;
}

std::optional<std::string> read_regular_file(const std::string &path, size_t max_bytes) {
	int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return std::nullopt;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
			static_cast<unsigned long long>(st.st_size) > max_bytes) {
		close(fd);
		return std::nullopt;
	}
	std::string out;
	char buf[8192];
	while (out.size() <= max_bytes) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		out.append(buf, static_cast<size_t>(n));
	}
	close(fd);
	if (out.size() > max_bytes)  /* grew while read */
		return std::nullopt;
	return out;
}

}  // namespace wb
