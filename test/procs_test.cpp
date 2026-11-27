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
