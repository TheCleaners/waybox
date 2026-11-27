/*
 * rc.xml <startup> programs: started in their own process group, reaped
 * on SIGCHLD, started again after a crash (see startup.hpp for the
 * policy), stopped when removed or disabled, ended when Waybox exits.
 */
#include "waybox/wlroots.hpp"

#include <algorithm>
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
#include "waybox/proc_group.hpp"

namespace {

struct Program {
	struct wb_server *server = nullptr;
	wb::StartupProgram prog;
	pid_t pid = -1;
	/* Its earlier copies that exited leaving something running in their
	 * process group (waypaper --restore leaves swaybg): kept unreaped, so
	 * the group's id (theirs) can't be reused while Waybox may still stop
	 * the group; reaped once nothing in it runs. */
	std::vector<pid_t> held;
	std::vector<int64_t> crashes;
	struct wl_event_source *restart_timer = nullptr;
	/* Restarted on request: started again once this one has exited (a
	 * second copy alongside would lose a single-instance lock, mako's bus
	 * name). */
	pid_t relaunch_after = -1;
	int64_t relaunch_since = 0;
	struct wl_event_source *relaunch_timer = nullptr;
	~Program() {
		if (restart_timer != nullptr)
			wl_event_source_remove(restart_timer);
		if (relaunch_timer != nullptr)
			wl_event_source_remove(relaunch_timer);
	}
};

int64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/* The leader (a child of ours, not reaped) if it has exited: looked at,
 * left unreaped. */
std::optional<siginfo_t> exited(pid_t pid) {
	siginfo_t si;
	memset(&si, 0, sizeof si);
	if (waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) != 0 ||
			si.si_pid != pid)
		return std::nullopt;
	return si;
}

void reap(pid_t pid) {
	while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
	}
}

/* A leader that exited, its group empty now: reaped (its id free). */
bool release_if_done(pid_t pid) {
	if (!exited(pid) || wb::group_has_others(pid, pid))
		return false;
	reap(pid);
	return true;
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
	/* Its own group from the start, also on this side of the fork: a stop
	 * before the child got to run must still reach it. */
	setpgid(pid, pid);
	return pid;
}

}  // namespace

namespace {
struct KillLater {
	struct wb_server *server;
	pid_t pid;
	bool killed = false;
	struct wl_event_source *timer = nullptr;
};
}  // namespace

struct wb_startup_state {
	std::vector<std::unique_ptr<Program>> programs;
	/* Stopped programs' leaders not reaped yet (running, or exited with
	 * their group still running: its id reserved till then), and their
	 * SIGKILL timers. */
	std::vector<pid_t> stopping;
	std::vector<KillLater *> kills;
	/* Looks now and then whether what held leaders left has ended. */
	struct wl_event_source *held_timer = nullptr;
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
static std::map<std::string, std::string> last_activation;  /* sent to D-Bus */

/* Set `env` for the programs started from now on, putting back what the
 * previous call set; then pass the changes to D-Bus and systemd. */
static void apply_env(struct wb_server *server,
		const std::vector<std::pair<std::string, std::string>> &env);

void startup_apply_env(struct wb_server *server) {
	static const std::vector<std::pair<std::string, std::string>> none;
	apply_env(server, server->config != nullptr ? server->config->input_method_env : none);
}

void startup_restore_env(struct wb_server *server) {
	/* Waybox is exiting: the shared systemd/D-Bus environment outlives it
	 * (and other desktops may start from it), so it gets back what it had
	 * before. */
	apply_env(server, {});
}

static void apply_env(struct wb_server *server,
		const std::vector<std::pair<std::string, std::string>> &env) {
	static std::map<std::string, std::optional<std::string>> before;
	for (const auto &[name, value] : before) {
		if (value)
			setenv(name.c_str(), value->c_str(), true);
		else
			unsetenv(name.c_str());
	}
	before.clear();
	for (const auto &[name, value] : env) {
		if (before.count(name) == 0) {
			const char *was = getenv(name.c_str());
			before[name] = was ? std::optional<std::string>(was) : std::nullopt;
		}
		setenv(name.c_str(), value.c_str(), true);
	}
	/* Programs D-Bus or systemd start (some opened from a file manager)
	 * get their environment from there, not from Waybox: send what changed
	 * since last time, names and values. A variable no longer set goes back
	 * to what Waybox has now (the session's, or cleared). Only plain names
	 * reach the shell command. */
	auto plain = [](const std::string &name) {
		return !name.empty() && name.find_first_not_of(
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
				std::string::npos;
	};
	std::map<std::string, std::string> now;
	for (const auto &[name, value] : env)
		if (plain(name))
			now[name] = value;
	std::string set, cleared;
	for (const auto &[name, value] : now) {
		auto it = last_activation.find(name);
		if (it == last_activation.end() || it->second != value)
			set += " " + name;
	}
	for (const auto &[name, value] : last_activation) {
		if (now.count(name))
			continue;
		if (getenv(name.c_str()) != nullptr)
			set += " " + name;  /* the session's value again */
		else
			cleared += " " + name;
	}
	last_activation = now;
	if (set.empty() && cleared.empty())
		return;
	std::string cmd = "command -v dbus-update-activation-environment >/dev/null && "
			"dbus-update-activation-environment --systemd" + set;
	for (size_t i = 0; i < cleared.size();) {
		size_t end = cleared.find(' ', i + 1);
		cmd += " " + cleared.substr(i + 1, end == std::string::npos ? std::string::npos
				: end - i - 1) + "=";
		i = end == std::string::npos ? cleared.size() : end;
	}
	if (!cleared.empty())
		cmd += "; systemctl --user unset-environment" + cleared + " 2>/dev/null";
	wlr_log(WLR_INFO, "wb-activation-env set:%s cleared:%s", set.c_str(), cleared.c_str());
	wb_spawn(cmd.c_str());
}

static void start(Program *p) {
	p->pid = spawn(p->prog.command);
	if (p->pid > 0)
		wlr_log(WLR_INFO, "wb-startup start %s %d", p->prog.name.c_str(), p->pid);
}


static int kill_later(void *data) {
	auto *later = static_cast<KillLater *>(data);
	struct wb_startup_state *st = later->server->startup;
	auto it = st != nullptr ? std::find(st->stopping.begin(), st->stopping.end(), later->pid)
			: std::vector<pid_t>::iterator();
	bool waiting = st != nullptr && it != st->stopping.end();
	if (waiting && !later->killed) {
		/* Still unreaped (running, or exited with its group running): the
		 * group's id is still this one's, never another's. */
		wlr_log(WLR_INFO, "wb-startup kill %d", later->pid);
		if (kill(-later->pid, SIGKILL) != 0)
			kill(later->pid, SIGKILL);
		later->killed = true;
		wl_event_source_timer_update(later->timer, 100);
		return 0;
	}
	if (waiting) {
		/* Killed: reaped once it has exited (what's left of its group is
		 * killed and never signalled again). */
		if (!exited(later->pid)) {
			wl_event_source_timer_update(later->timer, 500);
			return 0;
		}
		reap(later->pid);
		st->stopping.erase(it);
	}
	if (st != nullptr)
		st->kills.erase(std::remove(st->kills.begin(), st->kills.end(), later),
				st->kills.end());
	wl_event_source_remove(later->timer);
	delete later;
	return 0;
}

/* SIGTERM to a leader's group (or the leader, if the group isn't there);
 * SIGKILL in 3 s to what's still running then, the leader kept unreaped
 * till then so that the group's id stays its own. */
static void end_group(struct wb_startup_state *st, struct wb_server *server, pid_t pid) {
	if (kill(-pid, SIGTERM) != 0 && errno == ESRCH)
		kill(pid, SIGTERM);
	st->stopping.push_back(pid);
	auto *later = new KillLater{server, pid};
	st->kills.push_back(later);
	later->timer = wl_event_loop_add_timer(server->wl_event_loop, kill_later, later);
	wl_event_source_timer_update(later->timer, 3000);
}

/* The program, and what its earlier copies left running. */
static void stop(struct wb_startup_state *st, Program *p) {
	for (pid_t h : p->held) {
		wlr_log(WLR_INFO, "wb-startup stop %s (left running)", p->prog.name.c_str());
		end_group(st, p->server, h);
	}
	p->held.clear();
	if (p->pid <= 0)
		return;
	wlr_log(WLR_INFO, "wb-startup stop %s", p->prog.name.c_str());
	end_group(st, p->server, p->pid);
	p->pid = -1;
}

/* Held leaders whose groups have ended: reaped. Looked at every 5 s while
 * there are any. */
static int check_held(void *data) {
	auto *st = static_cast<struct wb_startup_state *>(data);
	bool any = false;
	for (auto &p : st->programs) {
		p->held.erase(std::remove_if(p->held.begin(), p->held.end(), release_if_done),
				p->held.end());
		any = any || !p->held.empty();
	}
	wl_event_source_timer_update(st->held_timer, any ? 5000 : 0);
	return 0;
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
	st->stopping.erase(std::remove_if(st->stopping.begin(), st->stopping.end(),
			release_if_done), st->stopping.end());
	for (auto &p : st->programs) {
		if (p->pid <= 0)
			continue;
		std::optional<siginfo_t> si = exited(p->pid);
		if (!si)
			continue;
		pid_t leader = p->pid;
		p->pid = -1;
		bool by_exit = si->si_code == CLD_EXITED;
		bool clean = by_exit && si->si_status == 0;
		wlr_log(WLR_INFO, "wb-startup exit %s %s %d", p->prog.name.c_str(),
				by_exit ? "status" : "signal", si->si_status);
		/* What it left running in its group is stopped with it later: its
		 * id kept from reuse till that has ended. */
		if (wb::group_has_others(leader, leader)) {
			p->held.push_back(leader);
			if (st->held_timer == nullptr)
				st->held_timer = wl_event_loop_add_timer(server->wl_event_loop,
						check_held, st);
			wl_event_source_timer_update(st->held_timer, 5000);
		} else {
			reap(leader);
		}
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

/* A restarted program's old copy: gone (it and what's left of its group,
 * a shell's trap still running), or 5 s on: started again. */
static int relaunch_check(void *data) {
	auto *p = static_cast<Program *>(data);
	struct wb_startup_state *st = p->server->startup;
	pid_t old = p->relaunch_after;
	/* Still stopping: unreaped, till its group has ended. */
	bool gone = true;
	if (st != nullptr) {
		auto it = std::find(st->stopping.begin(), st->stopping.end(), old);
		if (it != st->stopping.end() && release_if_done(old))
			st->stopping.erase(it);
		else
			gone = it == st->stopping.end();
	}
	if (gone || now_ms() - p->relaunch_since > 5000) {
		wl_event_source_remove(p->relaunch_timer);
		p->relaunch_timer = nullptr;
		p->relaunch_after = -1;
		start(p);
	} else {
		wl_event_source_timer_update(p->relaunch_timer, 50);
	}
	return 0;
}

/* Stop the program and start it again once it has exited: by its name, else
 * (named otherwise in rc.xml) by its command's program. */
std::string startup_restart(struct wb_server *server, const std::string &name,
		const std::string &binary) {
	struct wb_startup_state *st = server->startup;
	if (st == nullptr)
		return "Waybox didn't start it";
	Program *p = nullptr;
	for (auto &q : st->programs)
		if (q->prog.name == name)
			p = q.get();
	if (p == nullptr && !binary.empty())
		for (auto &q : st->programs) {
			std::string first = q->prog.command.substr(0,
					q->prog.command.find_first_of(" \t"));
			if (first.substr(first.rfind('/') + 1) == binary)
				p = q.get();
		}
	if (p == nullptr)
		return "Waybox didn't start it";
	if (p->restart_timer != nullptr) {
		wl_event_source_remove(p->restart_timer);
		p->restart_timer = nullptr;
	}
	p->crashes.clear();
	if (p->relaunch_after > 0)
		return "";  /* already on its way */
	wlr_log(WLR_INFO, "wb-startup restart %s (asked)", p->prog.name.c_str());
	if (p->pid <= 0) {
		stop(st, p);  /* what's left of its group */
		start(p);
		return "";
	}
	pid_t old = p->pid;
	stop(st, p);
	p->relaunch_after = old;
	p->relaunch_since = now_ms();
	p->relaunch_timer = wl_event_loop_add_timer(server->wl_event_loop, relaunch_check, p);
	wl_event_source_timer_update(p->relaunch_timer, 50);
	return "";
}

void startup_finish(struct wb_server *server) {
	struct wb_startup_state *st = server->startup;
	if (st == nullptr)
		return;
	/* Every group Waybox started ends with it: SIGTERM, up to 1.5 s for
	 * them to go, then SIGKILL to what's left; each leader kept unreaped
	 * till then, so that the group's id is still its own. */
	std::vector<pid_t> leaders = st->stopping;
	for (auto &p : st->programs) {
		if (p->pid > 0)
			leaders.push_back(p->pid);
		leaders.insert(leaders.end(), p->held.begin(), p->held.end());
	}
	for (pid_t l : leaders)
		if (kill(-l, SIGTERM) != 0)
			kill(l, SIGTERM);
	for (int64_t until = now_ms() + 1500; !leaders.empty() && now_ms() < until;) {
		leaders.erase(std::remove_if(leaders.begin(), leaders.end(), release_if_done),
				leaders.end());
		if (!leaders.empty())
			usleep(20 * 1000);
	}
	for (pid_t l : leaders) {
		wlr_log(WLR_INFO, "wb-startup kill %d (exiting)", l);
		if (kill(-l, SIGKILL) != 0)
			kill(l, SIGKILL);
	}
	/* Reaped once exited (killed, at once unless stuck in the kernel; not
	 * waited on past 0.5 s more). */
	for (int64_t until = now_ms() + 500; !leaders.empty() && now_ms() < until;) {
		leaders.erase(std::remove_if(leaders.begin(), leaders.end(), [](pid_t l) {
			if (!exited(l))
				return false;
			reap(l);
			return true;
		}), leaders.end());
		if (!leaders.empty())
			usleep(20 * 1000);
	}
	if (st->held_timer != nullptr)
		wl_event_source_remove(st->held_timer);
	for (KillLater *later : st->kills) {
		wl_event_source_remove(later->timer);
		delete later;
	}
	delete st;
	server->startup = nullptr;
}
