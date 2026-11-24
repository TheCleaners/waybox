#ifndef WB_STARTUP_HPP
#define WB_STARTUP_HPP

/*
 * Programs started with the session (rc.xml <waybox><startup>), as pure
 * logic: what Reconfigure has to stop and start, and when a program that
 * exited is started again. No wlroots; unit-tested.
 */
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/* <program name="panel" enabled="yes" restart="yes">waybar</program> */
struct StartupProgram {
	std::string name;     /* unique; defaults to the command's first word */
	std::string command;  /* run with /bin/sh -c */
	bool enabled = true;
	bool restart = true;  /* start it again if it crashes */
	bool operator==(const StartupProgram &) const = default;
};

/* The name a program is known by: its name, or its command's program name
 * ("waybar -c x" -> "waybar", "/usr/bin/mako" -> "mako"). */
std::string startup_name(std::string_view name, std::string_view command);

struct StartupPlan {
	std::vector<std::string> stop;          /* names */
	std::vector<StartupProgram> start;
};

/* From what's supervised now (`current`, started or not) to `wanted`:
 * stop what's gone, disabled or changed command; start what's new,
 * enabled again or changed. A program left as it was isn't touched (one
 * that already ran and exited isn't run again). Later duplicates of a
 * name in `wanted` are ignored. */
StartupPlan plan_startup(const std::vector<StartupProgram> &current,
		const std::vector<StartupProgram> &wanted);

/* After a program exited: in how many ms to start it again, or nothing.
 * A clean exit (status 0) isn't a crash: one-shot commands just finish.
 * Crashes back off (1 s, 2 s, 4 s ... up to 30 s), and after 5 crashes
 * within a minute it's given up on. `crashes` holds the times (ms) of
 * recent crashes and is updated. */
std::optional<int> restart_delay_ms(bool clean_exit, bool restart,
		std::vector<int64_t> &crashes, int64_t now_ms);

}  // namespace wb

#endif
