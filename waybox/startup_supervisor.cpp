/*
 * rc.xml <startup> programs: started in their own process group, reaped
 * on SIGCHLD, started again after a crash (see startup.hpp for the
 * policy), stopped when removed or disabled, ended when Waybox exits.
 */
#include "waybox/wlroots.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <map>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "config.h"
#include "startup_supervisor.h"
#include "waybox/server.h"
#include "waybox/startup.hpp"

namespace {

struct Program {
	struct wb_server *server = nullptr;
	wb::StartupProgram prog;
	pid_t pid = -1;
	std::vector<int64_t> crashes;
	struct wl_event_source *restart_timer = nullptr;
	~Program() {
		if (restart_timer != nullptr)
			wl_event_source_remove(restart_timer);
	}
};

int64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

pid_t spawn(const std::string &command) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* Its own process group, so stopping it ends what it started too;
		 * and not the compositor's blocked signals. */
		setsid();
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		execl("/bin/sh", "/bin/sh", "-c", command.c_str(), (char *) nullptr);
		_exit(127);
	}
	return pid;
}

}  // namespace

struct wb_startup_state {
	std::vector<std::unique_ptr<Program>> programs;
	/* Stopped programs not reaped yet. */
	std::vector<pid_t> stopping;
};

static struct wb_startup_state *state(struct wb_server *server) {
	if (server->startup == nullptr)
		server->startup = new wb_startup_state;
	return server->startup;
}

/* The <startup> programs, and the input method (supervised the same way,
 * under the name "input-method"). */
static std::vector<wb::StartupProgram> wanted_programs(struct wb_server *server) {
	std::vector<wb::StartupProgram> out = server->config->startup;
	if (!server->config->input_method.empty())
		out.insert(out.begin(), wb::StartupProgram{"input-method",
				server->config->input_method, true, true});
	return out;
}

/* The input method's variables, for the programs started from now on.
 * What they were before (from the session's environment file, or unset)
 * comes back when the input method no longer sets them. */
static std::string last_activation_names;

void startup_apply_env(struct wb_server *server) {
	static std::map<std::string, std::optional<std::string>> before;
	for (const auto &[name, value] : before) {
		if (value)
			setenv(name.c_str(), value->c_str(), true);
		else
			unsetenv(name.c_str());
	}
	before.clear();
	if (server->config == nullptr)
		return;  /* no settings loaded: no input method */
	for (const auto &[name, value] : server->config->input_method_env) {
		if (before.count(name) == 0) {
			const char *was = getenv(name.c_str());
			before[name] = was ? std::optional<std::string>(was) : std::nullopt;
		}
		setenv(name.c_str(), value.c_str(), true);
	}
	/* Programs D-Bus or systemd start (some opened from a file manager)
	 * get their environment from there, not from Waybox: pass the names on
	 * (with their values now, or unset). Names are checked to be plain. */
	std::string names;
	for (const auto &[name, value] : before) {
		bool plain = !name.empty() && name.find_first_not_of(
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
				std::string::npos;
		if (plain)
			names += " " + name;
	}
	if (!names.empty() && last_activation_names != names) {
		last_activation_names = names;
		wb_spawn(("command -v dbus-update-activation-environment >/dev/null && "
				"dbus-update-activation-environment --systemd" + names).c_str());
	}
}

static void start(Program *p) {
	p->pid = spawn(p->prog.command);
	if (p->pid > 0)
		wlr_log(WLR_INFO, "wb-startup start %s %d", p->prog.name.c_str(), p->pid);
}

static void stop(struct wb_startup_state *st, Program *p) {
	if (p->pid > 0) {
		wlr_log(WLR_INFO, "wb-startup stop %s", p->prog.name.c_str());
		kill(-p->pid, SIGTERM);
		st->stopping.push_back(p->pid);
		p->pid = -1;
	}
}

void startup_apply(struct wb_server *server) {
	if (server->config == nullptr)
		return;
	startup_apply_env(server);
	if (server->startup == nullptr && server->config->startup.empty() &&
			server->config->input_method.empty())
		return;  /* nothing listed, nothing ever started */
	struct wb_startup_state *st = state(server);
	std::vector<wb::StartupProgram> current;
	for (const auto &p : st->programs)
		current.push_back(p->prog);
	wb::StartupPlan plan = wb::plan_startup(current, wanted_programs(server));

	for (const std::string &name : plan.stop) {
		for (auto it = st->programs.begin(); it != st->programs.end(); ++it) {
			if ((*it)->prog.name == name) {
				stop(st, it->get());
				st->programs.erase(it);
				break;
			}
		}
	}
	/* Programs kept as they are still take a changed restart setting. */
	for (auto &p : st->programs)
		for (const wb::StartupProgram &w : wanted_programs(server))
			if (w.name == p->prog.name)
				p->prog.restart = w.restart;
	for (const wb::StartupProgram &w : plan.start) {
		auto p = std::make_unique<Program>();
		p->server = server;
		p->prog = w;
		start(p.get());
		st->programs.push_back(std::move(p));
	}
}

static int restart_due(void *data) {
	auto *p = static_cast<Program *>(data);
	wl_event_source_remove(p->restart_timer);
	p->restart_timer = nullptr;
	start(p);
	return 0;
}

void startup_reap(struct wb_server *server) {
	struct wb_startup_state *st = server->startup;
	if (st == nullptr)
		return;
	/* Only our own children: Xwayland and others are reaped by their
	 * owners. */
	for (auto it = st->stopping.begin(); it != st->stopping.end();) {
		if (waitpid(*it, nullptr, WNOHANG) != 0)
			it = st->stopping.erase(it);
		else
			++it;
	}
	for (auto &p : st->programs) {
		if (p->pid <= 0)
			continue;
		int status = 0;
		if (waitpid(p->pid, &status, WNOHANG) != p->pid)
			continue;
		p->pid = -1;
		bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		wlr_log(WLR_INFO, "wb-startup exit %s %s %d", p->prog.name.c_str(),
				WIFEXITED(status) ? "status" : "signal",
				WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
		auto delay = wb::restart_delay_ms(clean, p->prog.restart, p->crashes, now_ms());
		if (delay) {
			wlr_log(WLR_INFO, "wb-startup restart %s in %d ms", p->prog.name.c_str(), *delay);
			p->restart_timer = wl_event_loop_add_timer(server->wl_event_loop,
					restart_due, p.get());
			wl_event_source_timer_update(p->restart_timer, *delay);
		} else if (!clean && p->prog.restart) {
			wlr_log(WLR_ERROR, "wb-startup giving up %s: %s", p->prog.name.c_str(),
					"it keeps crashing");
		}
	}
}

void startup_finish(struct wb_server *server) {
	struct wb_startup_state *st = server->startup;
	if (st == nullptr)
		return;
	for (auto &p : st->programs)
		if (p->pid > 0)
			kill(-p->pid, SIGTERM);
	delete st;
	server->startup = nullptr;
}
