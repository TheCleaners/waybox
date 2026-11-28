#ifndef WB_AGENT_AUDIT_HPP
#define WB_AGENT_AUDIT_HPP

/*
 * The hub's audit log (doc/agent-tools.md, "The audit log"): one JSON line
 * per event, appended to $XDG_STATE_HOME/agents/tools/audit.log, the
 * user's alone (0600, in a 0700 folder). Lines are only ever added; past
 * AuditRotate the file becomes audit.log.1 (AuditKeep kept). Important
 * lines (intents, and what AuditSync names) are synced as written.
 *
 * At start, recover() ends each intent line with no ending as outcome
 * unknown: what a crash between dispatch and answer leaves. POSIX file
 * calls only; unit-tested.
 */
#include <set>
#include <string>
#include <vector>

#include "tunables.hpp"
#include "waybox/json.hpp"

namespace wb::agent {

class Audit {
public:
	/* `dir`: the audit's folder (made, only the user's). */
	Audit(std::string dir, const Tunables &t);
	~Audit();
	Audit(const Audit &) = delete;
	Audit &operator=(const Audit &) = delete;

	/* False (and why) if it can't be written: the hub then says so. */
	bool ok() const { return fd_ >= 0; }
	const std::string &error() const { return error_; }

	/* A line: "v" and "time" added. `important`: synced as written. */
	void write(json::Object line, bool important = false);
	/* Intents with no ending, ended as outcome unknown; how many. */
	size_t recover();

	/* Arguments as the audit keeps them: each `secrets` name's value
	 * redacted, strings past AuditCut cut and marked. */
	json::Value audit_args(const json::Value &args, const std::set<std::string> &secrets) const;

	/* "2026-11-28T10:01:02.123Z" (UTC). */
	static std::string now_iso();
	const std::string &path() const { return path_; }

private:
	std::string dir_, path_, error_;
	Tunables t_;
	int fd_ = -1;
	size_t size_ = 0;
	void open_file();
	void rotate();
};

}  // namespace wb::agent

#endif /* WB_AGENT_AUDIT_HPP */
