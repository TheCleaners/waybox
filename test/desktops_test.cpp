#include "wb_test.hpp"

#include "waybox/desktops.hpp"

using wb::desktop_name;
using wb::desktop_target;
using wb::parse_desktop_count;

WB_TEST(target_by_number_is_one_based) {
	WB_CHECK(desktop_target("1", 2, 0, 4, true) == 0);
	WB_CHECK(desktop_target("4", 0, 0, 4, true) == 3);
	WB_CHECK(!desktop_target("0", 0, 0, 4, true));
	WB_CHECK(!desktop_target("5", 0, 0, 4, true));
	WB_CHECK(!desktop_target("2x", 0, 0, 4, true));
	WB_CHECK(!desktop_target("bogus", 0, 0, 4, true));
}

WB_TEST(target_directions_wrap_only_when_asked) {
	WB_CHECK(desktop_target("next", 0, 0, 4, true) == 1);
	WB_CHECK(desktop_target("right", 3, 0, 4, true) == 0);
	WB_CHECK(desktop_target("right", 3, 0, 4, false) == 3);
	WB_CHECK(desktop_target("left", 0, 0, 4, true) == 3);
	WB_CHECK(desktop_target("previous", 0, 0, 4, false) == 0);
	WB_CHECK(desktop_target("left", 2, 0, 4, false) == 1);
	/* One row of desktops: vertical moves stay put. */
	WB_CHECK(desktop_target("up", 2, 0, 4, true) == 2);
	WB_CHECK(desktop_target("down", 2, 0, 4, true) == 2);
	/* A single desktop never moves. */
	WB_CHECK(desktop_target("right", 0, 0, 1, true) == 0);
}

WB_TEST(target_current_and_last) {
	WB_CHECK(desktop_target("current", 2, 1, 4, true) == 2);
	WB_CHECK(desktop_target("", 2, 1, 4, true) == 2);
	WB_CHECK(desktop_target("last", 2, 1, 4, true) == 1);
	/* A "last" desktop removed by a reconfigure falls back to the current. */
	WB_CHECK(desktop_target("last", 1, 6, 4, true) == 1);
	WB_CHECK(!desktop_target("1", 0, 0, 0, true));
}

WB_TEST(names_fall_back_to_numbers) {
	wb::DesktopConfig config;
	config.names = {"web", "", "mail"};
	WB_CHECK_EQ(desktop_name(config, 0), std::string("web"));
	WB_CHECK_EQ(desktop_name(config, 1), std::string("Desktop 2"));
	WB_CHECK_EQ(desktop_name(config, 2), std::string("mail"));
	WB_CHECK_EQ(desktop_name(config, 3), std::string("Desktop 4"));
}

WB_TEST(count_is_clamped) {
	WB_CHECK_EQ(parse_desktop_count("6", 4), 6);
	WB_CHECK_EQ(parse_desktop_count(" 2\n", 4), 2);
	WB_CHECK_EQ(parse_desktop_count("0", 4), 1);
	WB_CHECK_EQ(parse_desktop_count("1000", 4), wb::kMaxDesktops);
	WB_CHECK_EQ(parse_desktop_count("many", 4), 4);
	WB_CHECK_EQ(parse_desktop_count("", 4), 4);
}

WB_TEST(popup_time_parsing) {
	WB_CHECK(wb::parse_popup_time("875", 1) == 875);
	WB_CHECK(wb::parse_popup_time(" 0 ", 1) == 0);
	WB_CHECK(wb::parse_popup_time("99999", 1) == 10000);
	WB_CHECK(wb::parse_popup_time("-5", 1) == 0);
	WB_CHECK(wb::parse_popup_time("", 875) == 875);
	WB_CHECK(wb::parse_popup_time("soon", 875) == 875);
	WB_CHECK(wb::DesktopConfig{}.popup_time_ms == 875);  /* Openbox's default */
}
