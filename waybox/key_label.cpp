#include "waybox/key_label.hpp"

#include <utility>

namespace wb {

namespace {

/* Openbox's modifier letters. */
const char *modifier(std::string_view m) {
	if (m == "W" || m == "Mod4")
		return "Super";
	if (m == "A" || m == "Mod1")
		return "Alt";
	if (m == "C" || m == "Control")
		return "Ctrl";
	if (m == "S" || m == "Shift")
		return "Shift";
	if (m == "M")
		return "Meta";
	if (m == "H")
		return "Hyper";
	return nullptr;
}

std::string key_name(std::string_view k) {
	static const std::pair<const char *, const char *> names[] = {
		{"Left", "←"}, {"Right", "→"}, {"Up", "↑"}, {"Down", "↓"},
		{"Return", "Enter"}, {"KP_Enter", "Enter"}, {"space", "Space"}, {"Escape", "Esc"},
		{"BackSpace", "Backspace"}, {"Prior", "Page Up"}, {"Next", "Page Down"},
		{"period", "."}, {"comma", ","}, {"slash", "/"}, {"minus", "-"}, {"equal", "="},
		{"grave", "`"}, {"semicolon", ";"}, {"apostrophe", "'"}, {"backslash", "\\"},
		{"bracketleft", "["}, {"bracketright", "]"}, {"Print", "Print Screen"}, {"Sys_Req", "SysRq"},
		{"XF86AudioRaiseVolume", "Volume Up"}, {"XF86AudioLowerVolume", "Volume Down"},
		{"XF86AudioMute", "Mute"}, {"XF86AudioMicMute", "Mic Mute"},
		{"XF86AudioPlay", "Play"}, {"XF86AudioPause", "Pause"}, {"XF86AudioNext", "Next Track"},
		{"XF86AudioPrev", "Previous Track"}, {"XF86AudioStop", "Stop"},
		{"XF86MonBrightnessUp", "Brightness Up"}, {"XF86MonBrightnessDown", "Brightness Down"},
		{"XF86Calculator", "Calculator"}, {"XF86Mail", "Mail"}, {"XF86WWW", "Browser"},
		{"XF86Explorer", "Files"}, {"XF86PowerOff", "Power"}, {"XF86Sleep", "Sleep"},
	};
	for (const auto &[from, to] : names)
		if (k == from)
			return to;
	/* One letter: as printed on the key. */
	if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z')
		return std::string(1, static_cast<char>(k[0] - 'a' + 'A'));
	return std::string(k);
}

/* "W-S-Left" -> modifiers then the rest; a lone "-" (the minus key) kept. */
std::vector<std::string> split_mods(std::string_view s, std::string &rest) {
	std::vector<std::string> mods;
	while (true) {
		size_t dash = s.find('-');
		if (dash == std::string_view::npos || dash == 0 || dash + 1 >= s.size())
			break;
		const char *m = modifier(s.substr(0, dash));
		if (m == nullptr)
			break;
		mods.push_back(m);
		s.remove_prefix(dash + 1);
	}
	rest = std::string(s);
	return mods;
}

}  // namespace

std::vector<std::string> key_caps(std::string_view key) {
	std::string rest;
	std::vector<std::string> caps = split_mods(key, rest);
	caps.push_back(key_name(rest));
	return caps;
}

std::vector<std::string> button_caps(std::string_view button) {
	std::string rest;
	std::vector<std::string> caps = split_mods(button, rest);
	if (rest == "Left" || rest == "Middle" || rest == "Right")
		caps.push_back(rest + " button");
	else if (rest == "Up")
		caps.push_back("Scroll up");
	else if (rest == "Down")
		caps.push_back("Scroll down");
	else
		caps.push_back(rest);
	return caps;
}

std::string event_label(std::string_view e) {
	if (e == "DoubleClick")
		return "Double-click";
	return std::string(e);
}

std::string rc_key(const HeldModifiers &held, std::string_view k) {
	static const char *const lone[] = {"Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L",
			"Alt_R", "Super_L", "Super_R", "Meta_L", "Meta_R", "Hyper_L", "Hyper_R",
			"ISO_Level3_Shift", "Caps_Lock", "Num_Lock"};
	for (const char *m : lone)
		if (k == m)
			return "";
	if (k.empty())
		return "";
	std::string out;
	if (held.super)
		out += "W-";
	if (held.ctrl)
		out += "C-";
	if (held.alt)
		out += "A-";
	if (held.shift)
		out += "S-";
	std::string key(k);
	if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
		key[0] = static_cast<char>(key[0] - 'A' + 'a');
	return out + key;
}

std::string context_label(std::string_view c) {
	if (c == "Root" || c == "Desktop")
		return "Desktop";
	if (c == "Client")
		return "Window";
	if (c == "Frame")
		return "Anywhere on a window";
	if (c == "Titlebar" || c == "Title")
		return "Titlebar";
	return std::string(c);  /* Top, Left, BLCorner, Close, Iconify, ... */
}

}  // namespace wb
