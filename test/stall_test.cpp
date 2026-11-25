#include "wb_test.hpp"

#include "waybox/stall.hpp"

#include <vector>

WB_TEST(stalls_count_every_call_and_log_the_slow_ones) {
	wb::Stalls &s = wb::Stalls::get();
	s.reset();
	s.threshold_ms = 8;
	std::vector<std::string> logged;
	s.on_slow = [&logged](const std::string &n, double) { logged.push_back(n); };
	s.record("icon", 2);
	s.record("icon", 12);
	s.record("menu", 3);
	const wb::StallStat &icon = s.stats().at("icon");
	WB_CHECK(icon.count == 2 && icon.slow == 1 && icon.max_ms == 12 && icon.total_ms == 14);
	WB_CHECK(logged == std::vector<std::string>{"icon"});
	s.on_slow = nullptr;
}

WB_TEST(a_late_loop_names_the_longest_scope_since_the_last_tick) {
	wb::Stalls &s = wb::Stalls::get();
	s.reset();
	s.threshold_ms = 8;
	s.record("frame", 1);
	s.record("theme", 40);
	s.record("icon", 5);
	WB_CHECK(s.loop_late(45) == "theme");
	WB_CHECK(s.stats().at("loop").slow == 1 && s.stats().at("loop").max_ms == 45);
	/* The next tick starts afresh; a small lateness isn't counted. */
	WB_CHECK(s.loop_late(2).empty());
	WB_CHECK(s.stats().at("loop").count == 1);
}

WB_TEST(a_scope_times_its_block) {
	wb::Stalls &s = wb::Stalls::get();
	s.reset();
	{
		wb::StallScope t("work");
		double end = wb::monotonic_ms() + 3;
		while (wb::monotonic_ms() < end) {
		}
	}
	const wb::StallStat &w = s.stats().at("work");
	WB_CHECK(w.count == 1 && w.max_ms >= 3 && w.max_ms < 1000);
}
