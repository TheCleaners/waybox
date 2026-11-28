/*
 * Processes, the portable way: /proc on Linux, sysctl(KERN_PROC) on the
 * BSDs (where /proc isn't mounted by default). Names are cut to 15
 * characters everywhere (Linux's comm is), so they compare the same.
 * Zombies are left out: exited, whatever still holds their slot.
 */
#include "wbconf_ui.hpp"

#include <optional>
#include "waybox/platform.hpp"

#include <csignal>
#include <iterator>
#include <sstream>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <cerrno>
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
	/* Room for processes started meanwhile; more started than that
	 * (ENOMEM): asked again, rather than coming back with none. */
	std::vector<char> buf;
	size_t len = 0;
	bool read = false;
	for (int attempt = 0; attempt < 8 && !read; ++attempt) {
		if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0)
			return out;
		len += len / 4 + static_cast<size_t>(attempt) * len / 2;
		buf.resize(len);
		if (sysctl(mib, 3, buf.data(), &len, nullptr, 0) == 0)
			read = true;
		else if (errno != ENOMEM)
			return out;
	}
	if (!read)
		return out;
	for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len; off += sizeof(struct kinfo_proc)) {
		const auto *kp = reinterpret_cast<const struct kinfo_proc *>(buf.data() + off);
		if (kp->ki_stat == SZOMB)
			continue;  /* exited (Waybox may hold a startup program's leader) */
		out.push_back({kp->ki_pid, kp->ki_uid, cut(kp->ki_comm), kp->ki_ppid,
				static_cast<unsigned long long>(kp->ki_start.tv_sec) * 1000000ull +
						static_cast<unsigned long long>(kp->ki_start.tv_usec)});
	}
#elif defined(__linux__)
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
		if (!std::getline(f, comm))
			continue;
		/* The parent: the field after the state, past the ")" that ends
		 * the name (which may itself hold spaces or parentheses). */
		pid_t ppid = 0;
		std::string state;
		std::ifstream statf(dir + "/stat");
		std::string stat;
		std::getline(statf, stat);
		unsigned long long start = 0;
		if (size_t close = stat.rfind(')'); close != std::string::npos) {
			/* After the name: state, ppid, ... starttime (stat's field 22). */
			std::istringstream rest(stat.substr(close + 1));
			std::string field;
			rest >> state >> ppid;
			for (int f = 5; f < 22 && rest >> field; ++f) {
			}
			rest >> start;
		}
		/* Exited, not reaped yet (Waybox holds a startup program's leader
		 * while what it left runs): not running. */
		if (state == "Z" || state == "X")
			continue;
		out.push_back({static_cast<pid_t>(pid), st.st_uid, cut(comm), ppid, start});
	}
	closedir(d);
#else
	WB_PLATFORM_FALLBACK("listing processes (wbconf's running programs)");
#endif
	return out;
}

std::optional<ProcessInfo> process_info(pid_t pid) {
	for (const ProcessInfo &p : list_processes())
		if (p.pid == pid)
			return p;
	return std::nullopt;
}

bool signal_process(const ProcessInfo &p, int sig) {
	if (p.pid <= 1 || p.start == 0)
		return false;
	auto same = [&p]() {
		std::optional<ProcessInfo> now = process_info(p.pid);
		return now && now->start == p.start && now->uid == p.uid && now->name == p.name;
	};
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
	/* The process the pidfd holds can't be replaced: checked after it's
	 * opened, signalled through it. */
	int fd = static_cast<int>(syscall(SYS_pidfd_open, p.pid, 0));
	if (fd >= 0) {
		bool ok = same() && syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0) == 0;
		close(fd);
		return ok;
	}
	if (errno != ENOSYS)
		return false;  /* gone */
#endif
	/* No pidfd: checked, then signalled at once. */
	return same() && kill(p.pid, sig) == 0;
}

std::vector<std::string> process_args(pid_t pid) {
	std::string raw;
#if defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, static_cast<int>(pid)};
	size_t len = 0;
	if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0)
		return {};
	raw.resize(len);
	if (sysctl(mib, 4, raw.data(), &len, nullptr, 0) != 0)
		return {};
	raw.resize(len);
#elif defined(__linux__)
	std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
	raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
#else
	WB_PLATFORM_FALLBACK("another process's command line");
#endif
	std::vector<std::string> out;
	size_t start = 0;
	while (start < raw.size()) {
		size_t end = raw.find('\0', start);
		if (end == std::string::npos)
			end = raw.size();
		out.push_back(raw.substr(start, end - start));
		start = end + 1;
	}
	return out;
}

std::optional<std::string> process_env(pid_t pid, const std::string &name) {
	std::string raw;
#if defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ENV, static_cast<int>(pid)};
	size_t len = 0;
	if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0)
		return std::nullopt;
	raw.resize(len);
	if (sysctl(mib, 4, raw.data(), &len, nullptr, 0) != 0)
		return std::nullopt;
	raw.resize(len);
#elif defined(__linux__)
	std::ifstream f("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
	if (!f)
		return std::nullopt;
	raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
#else
	WB_PLATFORM_FALLBACK("another process's environment (which session it's in)");
	return std::nullopt;  /* unreadable: not this session's */
#endif
	std::string prefix = name + "=";
	size_t start = 0;
	while (start < raw.size()) {
		size_t end = raw.find('\0', start);
		if (end == std::string::npos)
			end = raw.size();
		if (raw.compare(start, prefix.size(), prefix) == 0)
			return raw.substr(start + prefix.size(), end - start - prefix.size());
		start = end + 1;
	}
	return std::string();  /* readable, not set */
}

bool in_this_session(pid_t pid) {
	/* Its Wayland display, as ours (one session's programs, not another's:
	 * a nested Waybox's, or the one around it). Unreadable: not ours. */
	for (const char *var : {"WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"}) {
		const char *mine = getenv(var);
		auto theirs = process_env(pid, var);
		if (!theirs || *theirs != (mine != nullptr ? mine : ""))
			return false;
	}
	return true;
}

std::string process_name(pid_t pid) {
#if defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
	struct kinfo_proc kp {};
	size_t len = sizeof(kp);
	if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len < sizeof(kp))
		return "";
	return cut(kp.ki_comm);
#elif defined(__linux__)
	std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
	std::string name;
	std::getline(comm, name);
	return cut(name);
#else
	WB_PLATFORM_FALLBACK("another process's name");
	return "";
#endif
}

}  // namespace wbconf
