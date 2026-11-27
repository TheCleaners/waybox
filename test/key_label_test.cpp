#include "wb_test.hpp"

#include "waybox/key_label.hpp"

using V = std::vector<std::string>;

WB_TEST(key_caps_read_as_printed) {
	WB_CHECK(wb::key_caps("W-S-Left") == (V{"Super", "Shift", "←"}));
	WB_CHECK(wb::key_caps("C-A-Delete") == (V{"Ctrl", "Alt", "Delete"}));
	WB_CHECK(wb::key_caps("W-d") == (V{"Super", "D"}));
	WB_CHECK(wb::key_caps("A-space") == (V{"Alt", "Space"}));
	WB_CHECK(wb::key_caps("XF86AudioRaiseVolume") == (V{"Volume Up"}));
	WB_CHECK(wb::key_caps("W-period") == (V{"Super", "."}));
	WB_CHECK(wb::key_caps("minus") == (V{"-"}));
	WB_CHECK(wb::key_caps("W-F1") == (V{"Super", "F1"}));
	/* Not a modifier: kept as written. */
	WB_CHECK(wb::key_caps("X-y") == (V{"X-y"}));
}

WB_TEST(buttons_events_contexts) {
	WB_CHECK(wb::button_caps("A-Left") == (V{"Alt", "Left button"}));
	WB_CHECK(wb::button_caps("S-Up") == (V{"Shift", "Scroll up"}));
	WB_CHECK(wb::event_label("DoubleClick") == "Double-click");
	WB_CHECK(wb::context_label("Root") == "Desktop");
	WB_CHECK(wb::context_label("Client") == "Window");
	WB_CHECK(wb::context_label("Close") == "Close");
}
