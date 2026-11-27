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
	/* Not a modifier Waybox knows: ignored, as Waybox does ("Mod4-x" is x). */
	WB_CHECK(wb::key_caps("X-y") == (V{"Y"}));
	WB_CHECK(wb::key_caps("Mod4-x") == (V{"X"}));
	/* Waybox's longer spellings. */
	WB_CHECK(wb::key_caps("Ctrl-Alt-Delete") == (V{"Ctrl", "Alt", "Delete"}));
	WB_CHECK(wb::key_caps("Logo-d") == (V{"Super", "D"}));
	WB_CHECK(wb::key_caps("S-exclam") == (V{"Shift", "!"}));
}

WB_TEST(buttons_events_contexts) {
	WB_CHECK(wb::button_caps("A-Left") == (V{"Alt", "Left button"}));
	WB_CHECK(wb::button_caps("S-Up") == (V{"Shift", "Scroll up"}));
	WB_CHECK(wb::event_label("DoubleClick") == "Double-click");
	WB_CHECK(wb::context_label("Root") == "Desktop");
	WB_CHECK(wb::context_label("Client") == "Window");
	/* Places Waybox doesn't read: shown as written, and not offered. */
	WB_CHECK(wb::context_label("Close") == "Close");
	for (const auto &[name, label] : wb::mouse_contexts())
		WB_CHECK(name != "Close" && name != "TLCorner" && name != "Maximize");
	WB_CHECK(wb::context_label("Desktop") == "Desktop and its icons");
	WB_CHECK(wb::context_label("Unknown") == "Unknown");
	std::string rest;
	wb::HeldModifiers h = wb::held_of("W-S-Left", rest);
	WB_CHECK(h.super && h.shift && !h.alt && !h.ctrl && rest == "Left");
	/* held_of fills `rest` first: its own statement (argument order is
	 * unspecified, and GCC reads `rest` before the call). */
	wb::HeldModifiers alt = wb::held_of("A-Middle", rest);
	WB_CHECK(wb::rc_key(alt, rest) == "A-Middle");
}

WB_TEST(rc_key_from_a_press) {
	wb::HeldModifiers super_shift;
	super_shift.super = true;
	super_shift.shift = true;
	WB_CHECK(wb::rc_key(super_shift, "Left") == "W-S-Left");
	wb::HeldModifiers ctrl_alt;
	ctrl_alt.ctrl = true;
	ctrl_alt.alt = true;
	WB_CHECK(wb::rc_key(ctrl_alt, "Delete") == "C-A-Delete");
	wb::HeldModifiers super;
	super.super = true;
	/* A letter as rc.xml writes it, whatever case the keyval had. */
	WB_CHECK(wb::rc_key(super, "D") == "W-d");
	WB_CHECK(wb::rc_key(super, "d") == "W-d");
	WB_CHECK(wb::rc_key({}, "F2") == "F2");
	/* A modifier on its own isn't a key yet. */
	WB_CHECK(wb::rc_key(super, "Super_L").empty());
	WB_CHECK(wb::rc_key({}, "Shift_R").empty());
	/* And what's written reads back as the caps pressed. */
	WB_CHECK(wb::key_caps(wb::rc_key(super_shift, "Left")) == (V{"Super", "Shift", "←"}));
}
