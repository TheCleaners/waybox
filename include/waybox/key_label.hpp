#ifndef WB_KEY_LABEL_HPP
#define WB_KEY_LABEL_HPP

/*
 * rc.xml's key and button names as people read them, for key caps:
 * "W-S-Left" -> {"Super", "Shift", "←"}, "C-A-Delete" -> {"Ctrl", "Alt",
 * "Delete"}, "XF86AudioRaiseVolume" -> {"Volume Up"}, "A-Left" (a mouse
 * button) -> {"Alt", "Left button"}. Pure (test/key_label_test.cpp).
 */
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb {

std::vector<std::string> key_caps(std::string_view key);
std::vector<std::string> button_caps(std::string_view button);
/* A mouse event as a phrase: "Click", "Double-click", "Press", "Drag". */
std::string event_label(std::string_view event);
/* A mouse context: "Titlebar", "the desktop" (Root), "a window" (Client). */
std::string context_label(std::string_view context);

/* A key pressed, as rc.xml writes it: the modifiers held, then the key's
 * name (xkb's, as gdk_keyval_name gives it): {super, shift}, "Left" ->
 * "W-S-Left"; one letter is lowercase ("W-d", not "W-D"). Empty for a
 * modifier on its own (Shift_L, Super_R, ...): not a key to bind yet. */
struct HeldModifiers {
	bool super = false, alt = false, ctrl = false, shift = false;
};
std::string rc_key(const HeldModifiers &held, std::string_view key_name);
/* The other way: "W-S-Left" -> {super, shift} and "Left" (the rest). */
HeldModifiers held_of(std::string_view key, std::string &rest);

/* Where a mouse binding applies, in the order offered: rc.xml's context
 * name and what it's called ("Close" -> "Close button"). */
const std::vector<std::pair<std::string, std::string>> &mouse_contexts();
/* The buttons and events offered, likewise ("Up" -> "Scroll up",
 * "DoubleClick" -> "Double-click"). */
const std::vector<std::pair<std::string, std::string>> &mouse_buttons();
const std::vector<std::pair<std::string, std::string>> &mouse_events();

}  // namespace wb

#endif /* WB_KEY_LABEL_HPP */
