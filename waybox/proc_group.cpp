#include "waybox/proc_group.hpp"

#include <cstdlib>
#include <string>

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <cerrno>
#include <vector>
#include <sys/sysctl.h>
#include <sys/user.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wb {

#if defined(__FreeBSD__) || defined(__DragonFly__)

bool group_has_others(pid_t pgid, pid_t except) {
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, static_cast<int>(pgid)};
	std::vector<char> buf;
	size_t len = 0;
	for (int attempt = 0; attempt < 8; ++attempt) {
		if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0)
			return false;
		len += len / 4 + 4 * sizeof(struct kinfo_proc);
		buf.resize(len);
		if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) == 0)
			break;
		if (errno != ENOMEM)
			return false;
		len = 0;
	}
	for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len; off += sizeof(struct kinfo_proc)) {
		const auto *kp = reinterpret_cast<const struct kinfo_proc *>(buf.data() + off);
		if (kp->ki_pid != except && kp->ki_pgid == pgid && kp->ki_stat != SZOMB)
			return true;
	}
	return false;
}

#else

bool group_has_others(pid_t pgid, pid_t except) {
	DIR *d = opendir("/proc");
	if (d == nullptr)
		return false;
	bool found = false;
	while (!found) {
		struct dirent *e = readdir(d);
		if (e == nullptr)
			break;
		char *end = nullptr;
		long pid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0' || pid <= 0 || pid == except)
			continue;
		std::string path = std::string("/proc/") + e->d_name + "/stat";
		int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		char buf[512];
		ssize_t n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		/* "pid (name) state ppid pgrp ...": past the last ")", as the
		 * name may hold spaces and parentheses. */
		std::string stat(buf, static_cast<size_t>(n));
		size_t close_paren = stat.rfind(')');
		if (close_paren == std::string::npos)
			continue;
		const char *p = stat.c_str() + close_paren + 1;
		while (*p == ' ')
			++p;
		char state = *p;
		if (state == '\0' || state == 'Z' || state == 'X')
			continue;
		++p;
		long ppid = strtol(p, &end, 10);
		(void) ppid;
		long pgrp = strtol(end, nullptr, 10);
		found = pgrp == pgid;
	}
	closedir(d);
	return found;
}

#endif

}  // namespace wb
