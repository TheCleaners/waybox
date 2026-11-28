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
	/* Calls (doc/agent-tools.md, "Calls"). */
	std::chrono::milliseconds call_timeout{30000};   /* reset by progress */
	std::chrono::milliseconds call_deadline{600000}; /* from dispatch, absolute */
	size_t max_in_flight = 8;                         /* per agent, and per provider */
	size_t call_rate = 600;                           /* calls a minute per agent */
	/* The audit (a distribution's to set: what's evidence isn't the user's
	 * to weaken unconfirmed, and the file is theirs either way). */
	size_t audit_cut = 4096;
	size_t audit_rotate = 10 * 1024 * 1024;
	size_t audit_keep = 3;
	std::string audit_sync = "important";  /* important, all, none */
};

/* [Tunables] from one file's text, over `t`; `system`: a distribution's
 * file (any value), else the user's (tightening only). What's ignored, and
 * why, goes to `notes`. */
void apply_tunables(Tunables &t, std::string_view ini_text, bool system,
		std::vector<std::string> &notes);

}  // namespace wb::agent

#endif /* WB_AGENT_TUNABLES_HPP */
