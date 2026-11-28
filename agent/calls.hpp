#ifndef WB_AGENT_CALLS_HPP
#define WB_AGENT_CALLS_HPP

/*
 * Every call an agent makes, through one set of states (doc/agent-tools.md,
 * "Calls"), on the hub's GLib main loop:
 *
 *   received → refused | dispatched (or waiting first, past MaxInFlight)
 *   dispatched → completed | cancellation-requested | outcome-unknown
 *
 * - Refused: past CallRate, or while the screen is locked; never sent.
 * - MaxInFlight per agent and per provider: more wait, in order.
 * - High-impact calls (exec, destroy, capture, input, Reach=outside) have
 *   an intent line in the audit, synced, before they're sent.
 * - CallTimeout (reset by each progress) and CallDeadline (from dispatch,
 *   absolute) end a call as outcome unknown, the agent told it may or may
 *   not have taken effect; the provider is asked to stop. Its slot stays
 *   taken until the provider answers or goes away: one that ignores the
 *   request to stop still gets at most MaxInFlight calls.
 * - Cancelled by the agent: waiting, it's cancelled (nothing was done);
 *   dispatched, the provider is asked to stop, and its answer, when it
 *   comes, is recorded, not sent. Likewise for an agent that leaves.
 * - Every call ends with a line in the audit.
 */
#include <functional>
#include <string>

#include "audit.hpp"
#include "catalog.hpp"
#include "tunables.hpp"
#include "waybox/json.hpp"

namespace wb::agent {

/* Who called, for the audit. */
struct Caller {
	unsigned conn = 0;
	std::string profile, label, claim;
};

/* Where a call's end goes: the agent's endpoint (each checks the
 * connection's still there). */
struct CallSink {
	std::function<void(json::Object result)> complete;
	std::function<void(int code, const std::string &message)> fail;
	std::function<void(double progress, const json::Value &total, const std::string &message)> progress;
};

void calls_init(const Tunables &t, Audit *audit);
void calls_stop();

/* A call to an offered tool (its level checked already); its id. */
std::string calls_begin(const Caller &who, const OfferedTool &tool, const json::Object &args,
		CallSink sink);
/* The agent cancelled it. */
void calls_cancel(const std::string &call);
/* The agent left: each of its calls as if cancelled, nothing sent to it. */
void calls_agent_left(unsigned conn);
/* A refusal by level, for the audit (the agent was told "unknown tool"). */
void calls_refused(const Caller &who, const OfferedTool &tool, const json::Object &args,
		const std::string &why);
/* The screen locked or unlocked: calls are refused meanwhile. */
void calls_set_locked(bool locked);

/* The text an agent gets for an outcome unknown. */
std::string outcome_unknown_text(const std::string &why);

}  // namespace wb::agent

#endif /* WB_AGENT_CALLS_HPP */
