#include "wb_test.hpp"

#include "../wbconf/wbconf_ui.hpp"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

static bool listed(pid_t pid) {
	for (const wbconf::ProcessInfo &p : wbconf::list_processes())
		if (p.pid == pid)
			return true;
	return false;
}

/* A process that has exited but isn't reaped yet (as Waybox holds a
 * startup program's leader while what it left runs) isn't running: not
 * listed. One that runs is. */
WB_TEST(processes_leave_out_zombies) {
	pid_t live = fork();
	if (live == 0) {
		pause();
		_exit(0);
	}
	pid_t dead = fork();
	if (dead == 0)
		_exit(0);
	/* Exited, left unreaped. */
	siginfo_t si{};
	waitid(P_PID, static_cast<id_t>(dead), &si, WEXITED | WNOWAIT);
	WB_CHECK(listed(live));
	WB_CHECK(!listed(dead));
	kill(live, SIGKILL);
	waitpid(live, nullptr, 0);
	waitpid(dead, nullptr, 0);
}

/* Signalled only if still that process: its start time (and owner and
 * name) as recorded. A record with another start time, as a reused id
 * would have, is refused; the right one signals it; one gone, nothing. */
WB_TEST(signal_only_the_same_process) {
	pid_t child = fork();
	if (child == 0) {
		pause();
		_exit(0);
	}
	usleep(50 * 1000);
	std::optional<wbconf::ProcessInfo> p = wbconf::process_info(child);
	WB_CHECK(p.has_value() && p->start != 0);
	if (p) {
		wbconf::ProcessInfo other = *p;
		other.start += 1;  /* a later process given the same id */
		WB_CHECK(!wbconf::signal_process(other, SIGTERM));
		WB_CHECK(kill(child, 0) == 0);  /* still there */
		WB_CHECK(wbconf::signal_process(*p, SIGTERM));
	}
	int status = 0;
	waitpid(child, &status, 0);
	WB_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
	if (p)
		WB_CHECK(!wbconf::signal_process(*p, SIGTERM));  /* gone */
}
