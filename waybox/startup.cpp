/*
 * Startup programs: the pure part (see startup.hpp).
 */
#include "waybox/startup.hpp"

#include <algorithm>
#include <set>

namespace wb {

namespace {
constexpr int kMaxCrashes = 5;
constexpr int64_t kCrashWindowMs = 60 * 1000;
constexpr int kMaxDelayMs = 30 * 1000;
}  // namespace

std::string startup_name(std::string_view name, std::string_view command) {
	auto trim = [](std::string_view s) {
		size_t b = s.find_first_not_of(" \t\r\n");
		if (b == std::string_view::npos)
			return std::string_view{};
		size_t e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	};
	name = trim(name);
	if (!name.empty())
		return std::string(name);
	command = trim(command);
	std::string_view first = command.substr(0, command.find_first_of(" \t"));
	size_t slash = first.rfind('/');
	if (slash != std::string_view::npos)
		first = first.substr(slash + 1);
	return std::string(first);
}

StartupPlan plan_startup(const std::vector<StartupProgram> &current,
		const std::vector<StartupProgram> &wanted) {
	StartupPlan plan;
	std::vector<const StartupProgram *> want;
	std::set<std::string> seen;
	for (const StartupProgram &p : wanted) {
		if (p.name.empty() || !seen.insert(p.name).second)
			continue;
		if (p.enabled && !p.command.empty())
			want.push_back(&p);
	}
	auto find_want = [&](const std::string &name) -> const StartupProgram * {
		for (const StartupProgram *p : want)
			if (p->name == name)
				return p;
		return nullptr;
	};
	auto find_current = [&](const std::string &name) -> const StartupProgram * {
		for (const StartupProgram &p : current)
			if (p.name == name)
				return &p;
		return nullptr;
	};
	for (const StartupProgram &p : current) {
		const StartupProgram *w = find_want(p.name);
		if (w == nullptr || w->command != p.command)
			plan.stop.push_back(p.name);
	}
	for (const StartupProgram *w : want) {
		const StartupProgram *c = find_current(w->name);
		if (c == nullptr || c->command != w->command)
			plan.start.push_back(*w);
	}
	return plan;
}

std::optional<int> restart_delay_ms(bool clean_exit, bool restart,
		std::vector<int64_t> &crashes, int64_t now_ms) {
	if (clean_exit || !restart)
		return std::nullopt;
	crashes.erase(std::remove_if(crashes.begin(), crashes.end(),
			[now_ms](int64_t t) { return now_ms - t > kCrashWindowMs; }),
			crashes.end());
	crashes.push_back(now_ms);
	int n = static_cast<int>(crashes.size());
	if (n >= kMaxCrashes)
		return std::nullopt;
	int delay = 1000 << (n - 1);
	return std::min(delay, kMaxDelayMs);
}

}  // namespace wb
