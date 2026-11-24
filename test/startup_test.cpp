#include "wb_test.hpp"

#include "waybox/startup.hpp"

using wb::StartupProgram;

namespace {
StartupProgram prog(const char *name, const char *cmd, bool enabled = true) {
	return StartupProgram{name, cmd, enabled, true};
}
bool has(const std::vector<std::string> &v, const char *s) {
	for (const auto &x : v)
		if (x == s)
			return true;
	return false;
}
}  // namespace

WB_TEST(startup_name_defaults_to_the_program) {
	WB_CHECK(wb::startup_name("panel", "waybar") == "panel");
	WB_CHECK(wb::startup_name("", "waybar -c x") == "waybar");
	WB_CHECK(wb::startup_name("  ", " /usr/bin/mako ") == "mako");
	WB_CHECK(wb::startup_name("", "").empty());
}

WB_TEST(plan_from_nothing_starts_enabled_programs) {
	auto plan = wb::plan_startup({}, {prog("panel", "waybar"),
			prog("night", "wlsunset", false), prog("panel", "yambar")});
	WB_CHECK(plan.stop.empty());
	WB_CHECK(plan.start.size() == 1);  /* disabled and duplicate left out */
	WB_CHECK(plan.start[0].command == "waybar");
}

WB_TEST(plan_reconfigure_touches_only_what_changed) {
	std::vector<StartupProgram> now = {prog("panel", "waybar"),
			prog("notes", "mako"), prog("night", "wlsunset -t 4000"),
			prog("once", "systemctl --user try-restart x")};
	std::vector<StartupProgram> want = {prog("panel", "waybar"),
			prog("notes", "mako", false), prog("night", "wlsunset -t 3500"),
			prog("once", "systemctl --user try-restart x"),
			prog("wall", "swaybg -c '#333'")};
	auto plan = wb::plan_startup(now, want);
	WB_CHECK(plan.stop.size() == 2);
	WB_CHECK(has(plan.stop, "notes") && has(plan.stop, "night"));
	WB_CHECK(plan.start.size() == 2);
	WB_CHECK(plan.start[0].name == "night" && plan.start[0].command == "wlsunset -t 3500");
	WB_CHECK(plan.start[1].name == "wall");
}

WB_TEST(plan_removed_program_is_stopped) {
	auto plan = wb::plan_startup({prog("panel", "waybar")}, {});
	WB_CHECK(plan.stop.size() == 1 && plan.stop[0] == "panel");
	WB_CHECK(plan.start.empty());
}

WB_TEST(restart_only_after_crashes_with_backoff) {
	std::vector<int64_t> crashes;
	WB_CHECK(!wb::restart_delay_ms(true, true, crashes, 0));    /* clean exit */
	WB_CHECK(!wb::restart_delay_ms(false, false, crashes, 0));  /* restart="no" */
	WB_CHECK(crashes.empty());
	WB_CHECK(wb::restart_delay_ms(false, true, crashes, 1000) == 1000);
	WB_CHECK(wb::restart_delay_ms(false, true, crashes, 3000) == 2000);
	WB_CHECK(wb::restart_delay_ms(false, true, crashes, 6000) == 4000);
	WB_CHECK(wb::restart_delay_ms(false, true, crashes, 11000) == 8000);
	/* the fifth crash within a minute: given up on */
	WB_CHECK(!wb::restart_delay_ms(false, true, crashes, 20000));
}

WB_TEST(restart_crashes_age_out) {
	std::vector<int64_t> crashes;
	for (int i = 0; i < 4; ++i)
		wb::restart_delay_ms(false, true, crashes, i * 1000);
	/* well over a minute later: counts from one again */
	WB_CHECK(wb::restart_delay_ms(false, true, crashes, 200000) == 1000);
}
