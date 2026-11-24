#include "wbconf_ui.hpp"

namespace wbconf {

/* A blank rc.xml used only when the user has no config and none is installed. */
const char *kSkeletonRc =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"</openbox_config>\n";

std::string home_dir() {
	if (const char *h = getenv("HOME"); h && h[0])
		return h;
	return {};
}

/* Where wbconf writes: $XDG_CONFIG_HOME/waybox/rc.xml, else ~/.config/...  */
std::string user_rc_path() {
	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
		return (fs::path(xdg) / "waybox" / "rc.xml").string();
	std::string h = home_dir();
	if (!h.empty())
		return (fs::path(h) / ".config" / "waybox" / "rc.xml").string();
	return {};
}

/* The system default, used to seed a brand-new user config. */
std::string system_rc_path() {
#ifdef WB_SYSCONFDIR
	return (fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "rc.xml").string();
#else
	return {};
#endif
}

/* Load the document to edit: the user's file if present, else the system
 * default (so the user inherits its settings), else a blank skeleton. A user
 * file with an error is NOT replaced by the default: nullopt, with the error,
 * so nothing is saved over it. */
std::optional<wb::RcDocument> load_document(std::string *problem) {
	std::error_code ec;
	std::string up = user_rc_path();
	if (!up.empty() && fs::exists(up, ec)) {
		wb::LoadError err;
		if (auto d = wb::RcDocument::load_file(up, &err))
			return d;
		if (problem != nullptr) {
			char *msg = g_strdup_printf(_("%s, line %d: %s. Fix it (wbconf won't save "
					"over it), then reopen wbconf."), up.c_str(), err.line,
					err.message.c_str());
			*problem = msg;
			g_free(msg);
		}
		return std::nullopt;
	}
	std::string sp = system_rc_path();
	if (!sp.empty() && fs::exists(sp, ec))
		if (auto d = wb::RcDocument::load_file(sp))
			return d;
	return wb::RcDocument::from_string(kSkeletonRc);
}

/* The process at the other end of this session's Wayland socket: the
 * compositor wbconf is running in. 0 if there is none. */
pid_t session_compositor() {
	const char *display = getenv("WAYLAND_DISPLAY");
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (display == nullptr || display[0] == '\0')
		return 0;
	std::string path = display[0] == '/' ? display
			: (runtime ? std::string(runtime) + "/" + display : std::string());
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (path.empty() || path.size() >= sizeof(addr.sun_path))
		return 0;
	std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	pid_t pid = 0;
	struct ucred cred = {};
	socklen_t len = sizeof(cred);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 &&
			getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
		pid = cred.pid;
	close(fd);
	return pid;
}

bool is_waybox(long pid) {
	std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
	std::string name;
	return std::getline(comm, name) && name == "waybox";
}

/* Ask Waybox to reconfigure (SIGUSR2, the signal Openbox uses): the one
 * wbconf runs in; outside a Waybox session, every Waybox this user runs.
 * Returns the number signalled. */
int signal_waybox_reload() {
	/* The Waybox that started us (it exports its pid), then the one
	 * serving our Wayland display, and only then any of ours. */
	if (const char *env = getenv("WAYBOX_PID")) {
		long pid = strtol(env, nullptr, 10);
		if (pid > 1 && is_waybox(pid))
			return kill(static_cast<pid_t>(pid), SIGUSR2) == 0 ? 1 : 0;
	}
	if (pid_t pid = session_compositor(); pid > 0 && is_waybox(pid))
		return kill(pid, SIGUSR2) == 0 ? 1 : 0;
	int signalled = 0;
	uid_t me = getuid();
	DIR *proc = opendir("/proc");
	if (proc == nullptr)
		return 0;
	struct dirent *e;
	while ((e = readdir(proc)) != nullptr) {
		char *end = nullptr;
		long pid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0' || pid <= 0)
			continue;
		if (!is_waybox(pid))
			continue;
		struct stat st {};
		std::string dir = std::string("/proc/") + e->d_name;
		if (stat(dir.c_str(), &st) != 0 || st.st_uid != me)
			continue;
		if (kill(static_cast<pid_t>(pid), SIGUSR2) == 0)
			++signalled;
	}
	closedir(proc);
	return signalled;
}

}  // namespace wbconf
