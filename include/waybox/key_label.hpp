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
#include <vector>

namespace wb {

std::vector<std::string> key_caps(std::string_view key);
std::vector<std::string> button_caps(std::string_view button);
/* A mouse event as a phrase: "Click", "Double-click", "Press", "Drag". */
std::string event_label(std::string_view event);
/* A mouse context: "Titlebar", "the desktop" (Root), "a window" (Client). */
std::string context_label(std::string_view context);

}  // namespace wb

#endif /* WB_KEY_LABEL_HPP */
