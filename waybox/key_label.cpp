#include "waybox/key_label.hpp"

#include <utility>

namespace wb {

namespace {

/* The modifier tokens Waybox reads (wb_parse_key_combo, and the mouse's
 * modifier_from_token): exactly those, so what's shown and compared is
 * what Waybox binds. Anything else before the key, Waybox ignores. */
const char *modifier(std::string_view m) {
	if (m == "W" || m == "Logo")
		return "Super";
	if (m == "A" || m == "Alt")
		return "Alt";
	if (m == "C" || m == "Ctrl")
		return "Ctrl";
	if (m == "S" || m == "Shift")
		return "Shift";
	if (m == "Caps")
		return "Caps Lock";
	if (m == "Mod2")
		return "Num Lock";
	if (m == "Mod3")
		return "Mod3";
	if (m == "Mod5")
		return "Mod5";
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
		{"exclam", "!"}, {"at", "@"}, {"numbersign", "#"}, {"dollar", "$"}, {"percent", "%"},
		{"asciicircum", "^"}, {"ampersand", "&"}, {"asterisk", "*"}, {"parenleft", "("},
		{"parenright", ")"}, {"underscore", "_"}, {"plus", "+"}, {"colon", ":"},
		{"quotedbl", "\""}, {"less", "<"}, {"greater", ">"}, {"question", "?"}, {"bar", "|"},
		{"braceleft", "{"}, {"braceright", "}"}, {"asciitilde", "~"},
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

/* "W-S-Left" -> modifiers then the key, as Waybox splits it: at every
 * "-", the last part the key, the others modifiers if known (unknown ones
 * ignored, as Waybox does: "Mod4-x" is x). A lone "-" kept as it is. */
std::vector<std::string> split_mods(std::string_view s, std::string &rest) {
	std::vector<std::string_view> parts;
	size_t at = 0;
	while (at <= s.size()) {
		size_t dash = s.find('-', at);
		std::string_view part = s.substr(at, dash == std::string_view::npos ? s.npos : dash - at);
		if (!part.empty())
			parts.push_back(part);
		if (dash == std::string_view::npos)
			break;
		at = dash + 1;
	}
	std::vector<std::string> mods;
	if (parts.empty()) {
		rest = std::string(s);
		return mods;
	}
	for (size_t i = 0; i + 1 < parts.size(); ++i)
		if (const char *m = modifier(parts[i]))
			mods.push_back(m);
	rest = std::string(parts.back());
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
	if (held.caps)
		out += "Caps-";
	if (held.mod2)
		out += "Mod2-";
	if (held.mod3)
		out += "Mod3-";
	if (held.mod5)
		out += "Mod5-";
	std::string key(k);
	if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
		key[0] = static_cast<char>(key[0] - 'A' + 'a');
	return out + key;
}

HeldModifiers held_of(std::string_view key, std::string &rest) {
	HeldModifiers held;
	for (const std::string &m : split_mods(key, rest)) {
		held.super = held.super || m == "Super";
		held.alt = held.alt || m == "Alt";
		held.ctrl = held.ctrl || m == "Ctrl";
		held.shift = held.shift || m == "Shift";
		held.caps = held.caps || m == "Caps Lock";
		held.mod2 = held.mod2 || m == "Num Lock";
		held.mod3 = held.mod3 || m == "Mod3";
		held.mod5 = held.mod5 || m == "Mod5";
	}
	return held;
}

const std::vector<std::pair<std::string, std::string>> &mouse_contexts() {
	static const std::vector<std::pair<std::string, std::string>> all = {
		/* The places Waybox reads (mouse_context_from_name), no others. */
		{"Root", "Desktop"}, {"Desktop", "Desktop and its icons"}, {"Client", "Window"},
		{"Frame", "Anywhere on a window"}, {"Titlebar", "Titlebar"}, {"Icon", "Window icon"},
		{"Top", "Top edge"}, {"Bottom", "Bottom edge"}, {"Left", "Left edge"},
		{"Right", "Right edge"},
	};
	return all;
}

const std::vector<std::pair<std::string, std::string>> &mouse_buttons() {
	static const std::vector<std::pair<std::string, std::string>> all = {
		{"Left", "Left button"}, {"Middle", "Middle button"}, {"Right", "Right button"},
		{"Up", "Scroll up"}, {"Down", "Scroll down"},
	};
	return all;
}

const std::vector<std::pair<std::string, std::string>> &mouse_events() {
	static const std::vector<std::pair<std::string, std::string>> all = {
		{"Press", "Press"}, {"Click", "Click"}, {"DoubleClick", "Double-click"},
		{"Release", "Release"}, {"Drag", "Drag"},
	};
	return all;
}

std::string context_label(std::string_view c) {
	if (c == "Title")
		return "Titlebar";
	for (const auto &[name, label] : mouse_contexts())
		if (c == name)
			return label;
	return std::string(c);
}

}  // namespace wb
