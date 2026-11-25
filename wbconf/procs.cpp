/*
 * Processes, the portable way: /proc on Linux, sysctl(KERN_PROC) on the
 * BSDs (where /proc isn't mounted by default). Names are cut to 15
 * characters everywhere (Linux's comm is), so they compare the same.
 */
#include "wbconf_ui.hpp"

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

namespace wbconf {

namespace {
std::string cut(std::string s) {
	if (s.size() > 15)
		s.resize(15);
	return s;
}
}  // namespace

std::vector<ProcessInfo> list_processes() {
	std::vector<ProcessInfo> out;
#if defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
	size_t len = 0;
	if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0)
		return out;
	len += len / 4;  /* room for processes started meanwhile */
	std::vector<char> buf(len);
	if (sysctl(mib, 3, buf.data(), &len, nullptr, 0) != 0)
		return out;
	for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len; off += sizeof(struct kinfo_proc)) {
		const auto *kp = reinterpret_cast<const struct kinfo_proc *>(buf.data() + off);
		out.push_back({kp->ki_pid, kp->ki_uid, cut(kp->ki_comm)});
	}
#else
	DIR *d = opendir("/proc");
	if (d == nullptr)
		return out;
	while (struct dirent *e = readdir(d)) {
		char *end = nullptr;
		long pid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0' || pid <= 0)
			continue;
		std::string dir = std::string("/proc/") + e->d_name;
		struct stat st {};
		if (stat(dir.c_str(), &st) != 0)
			continue;
		std::ifstream f(dir + "/comm");
		std::string comm;
		if (std::getline(f, comm))
			out.push_back({static_cast<pid_t>(pid), st.st_uid, cut(comm)});
	}
	closedir(d);
#endif
	return out;
}

std::string process_name(pid_t pid) {
#if defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
	struct kinfo_proc kp {};
	size_t len = sizeof(kp);
	if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len < sizeof(kp))
		return "";
	return cut(kp.ki_comm);
#else
	std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
	std::string name;
	std::getline(comm, name);
	return cut(name);
#endif
}

}  // namespace wbconf
