#ifndef WB_AGENT_TUNABLES_HPP
#define WB_AGENT_TUNABLES_HPP

/*
 * The hub's tunables (doc/agent-tools.md, "Tunables"): the suggested
 * values, changed by the [Tunables] of settings.ini. A distribution's file
 * (the system's) may set any value. The user's may only tighten a guard
 * for now: weakening one needs the compositor's confirmation, which comes
 * with the confirmed settings, so until then it's ignored and noted.
 *
 * Values are a number and a unit as the spec writes them: sizes in B, KiB
 * or MiB, durations in ms, s or min, counts bare. Pure; unit-tested.
 */
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wb::agent {

struct Tunables {
	size_t max_frame = 4 * 1024 * 1024;
	size_t max_queued = 8 * 1024 * 1024;
	size_t max_agents = 32;
	size_t max_listens = 8;
	std::chrono::milliseconds handshake_wait{10000};
};

/* [Tunables] from one file's text, over `t`; `system`: a distribution's
 * file (any value), else the user's (tightening only). What's ignored, and
 * why, goes to `notes`. */
void apply_tunables(Tunables &t, std::string_view ini_text, bool system,
		std::vector<std::string> &notes);

}  // namespace wb::agent

#endif /* WB_AGENT_TUNABLES_HPP */
