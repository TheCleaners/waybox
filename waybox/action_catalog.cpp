#include "waybox/action_catalog.hpp"

#include "waybox/action.hpp"

namespace wb {

namespace {

using C = std::vector<std::pair<std::string, std::string>>;

const C kDirections = {{"north", "Up"}, {"south", "Down"}, {"east", "Right"}, {"west", "Left"}};
const C kDirections8 = {{"north", "Up"}, {"northeast", "Up and right"}, {"east", "Right"},
		{"southeast", "Down and right"}, {"south", "Down"}, {"southwest", "Down and left"},
		{"west", "Left"}, {"northwest", "Up and left"}};
const C kSnap = {{"left", "Left half"}, {"right", "Right half"}, {"up", "Maximized"},
		{"up-left", "Top left quarter"}, {"up-right", "Top right quarter"},
		{"down-left", "Bottom left quarter"}, {"down-right", "Bottom right quarter"},
		{"down", "Back as it was"}};
const C kMaximize = {{"both", "Both ways"}, {"horizontal", "Across"}, {"vertical", "Up and down"}};

ActionOption to_desktop() {
	return {"to", "Desktop", OptionKind::Text, {}, "1"};  /* a number, left, right, last */
}
ActionOption wrap() {
	return {"wrap", "Wrap around", OptionKind::Bool, {}, "yes"};
}
ActionOption dir(const C &choices, std::string fallback = "") {
	return {"direction", "Direction", OptionKind::Choice, choices, std::move(fallback)};
}
ActionOption number(const char *name, const char *label) {
	return {name, label, OptionKind::Text, {}, ""};  /* pixels, N%, center, current */
}

}  // namespace

const std::vector<ActionInfo> &action_catalog() {
	static const std::vector<ActionInfo> catalog = {
		/* Programs and menus */
		{"Execute", "Run a program", "Programs and menus",
				{{"command", "Command", OptionKind::Command, {}, ""}}},
		{"ShowMenu", "Show a menu", "Programs and menus",
				{{"menu", "Menu", OptionKind::Text, {}, "root-menu"}}},
		{"ShowAbout", "About Waybox", "Programs and menus", {}},
		{"Reconfigure", "Reload the settings", "Programs and menus", {}},
		{"Exit", "Log out", "Programs and menus", {}},
		/* Windows */
		{"Close", "Close the window", "Windows", {}},
		{"Kill", "End the window's program", "Windows", {}},
		{"Iconify", "Minimize", "Windows", {}},
		{"ToggleMaximize", "Maximize or restore", "Windows", {dir(kMaximize, "both")}},
		{"Maximize", "Maximize", "Windows", {dir(kMaximize, "both")}},
		{"Unmaximize", "Restore from maximized", "Windows", {dir(kMaximize, "both")}},
		{"ToggleFullscreen", "Fullscreen or back", "Windows", {}},
		{"ToggleShade", "Roll up or down", "Windows", {}},
		{"Shade", "Roll up", "Windows", {}},
		{"Unshade", "Roll down", "Windows", {}},
		{"ShadeLower", "Roll up, or lower", "Windows", {}},
		{"UnshadeRaise", "Roll down, or raise", "Windows", {}},
		{"ToggleDecorations", "Frame on or off", "Windows", {}},
		{"Decorate", "Frame on", "Windows", {}},
		{"Undecorate", "Frame off", "Windows", {}},
		{"Raise", "Raise", "Windows", {}},
		{"Lower", "Lower", "Windows", {}},
		{"RaiseLower", "Raise or lower", "Windows", {}},
		{"ToggleAlwaysOnTop", "Always on top, or not", "Windows", {}},
		{"ToggleAlwaysOnBottom", "Always on bottom, or not", "Windows", {}},
		{"SendToTopLayer", "Always on top", "Windows", {}},
		{"SendToNormalLayer", "With the others", "Windows", {}},
		{"SendToBottomLayer", "Always on bottom", "Windows", {}},
		/* Moving and sizing */
		{"Move", "Move with the mouse", "Moving and sizing", {}},
		{"Resize", "Resize with the mouse", "Moving and sizing", {}},
		{"SnapToEdge", "Tile to an edge", "Moving and sizing", {dir(kSnap, "left")}},
		{"MoveToCenter", "Center on the monitor", "Moving and sizing", {}},
		{"MoveResizeTo", "Move and size to", "Moving and sizing",
				{number("x", "Left"), number("y", "Top"), number("width", "Width"),
				 number("height", "Height"), {"monitor", "Monitor", OptionKind::Text, {}, "current"}}},
		{"MoveRelative", "Move by", "Moving and sizing", {number("x", "Across"),
				number("y", "Down")}},
		{"ResizeRelative", "Grow or shrink by", "Moving and sizing", {number("left", "Left"),
				number("right", "Right"), number("top", "Top"), number("bottom", "Bottom")}},
		{"MoveToEdge", "Move to the next edge", "Moving and sizing", {dir(kDirections)}},
		{"GrowToEdge", "Grow to the next edge", "Moving and sizing", {dir(kDirections)}},
		{"ShrinkToEdge", "Shrink to the next edge", "Moving and sizing", {dir(kDirections)}},
		{"GrowToFill", "Fill the free space", "Moving and sizing", {}},
		/* Focus */
		{"NextWindow", "Next window (Alt+Tab)", "Focus", {}},
		{"PreviousWindow", "Previous window", "Focus", {}},
		{"DirectionalTargetWindow", "Focus the window that way", "Focus",
				{dir(kDirections8)}},
		{"Focus", "Focus the window", "Focus", {}},
		{"Unfocus", "Unfocus", "Focus", {}},
		{"FocusFallback", "Focus the last window", "Focus", {}},
		{"FocusToBottom", "Last in Alt+Tab", "Focus", {}},
		/* Desktops */
		{"GoToDesktop", "Go to desktop", "Desktops", {to_desktop(), wrap()}},
		{"SendToDesktop", "Send to desktop", "Desktops", {to_desktop(), wrap(),
				{"follow", "Go with it", OptionKind::Bool, {}, "yes"}}},
		{"ToggleOmnipresent", "On all desktops, or one", "Desktops", {}},
		{"ToggleShowDesktop", "Show the desktop", "Desktops", {}},
		{"AddDesktop", "Add a desktop", "Desktops", {{"where", "Where", OptionKind::Choice,
				{{"last", "At the end"}, {"current", "After this one"}}, "last"}}},
		{"RemoveDesktop", "Remove a desktop", "Desktops", {{"where", "Which",
				OptionKind::Choice, {{"last", "The last"}, {"current", "This one"}}, "last"}}},
	};
	return catalog;
}

const ActionInfo *action_info(std::string_view name) {
	for (const ActionInfo &a : action_catalog())
		if (a.name == name)
			return &a;
	/* An alias: the entry of the same kind. */
	const ActionSpec *spec = action_spec_from_name(name);
	if (spec == nullptr)
		return nullptr;
	for (const ActionInfo &a : action_catalog()) {
		const ActionSpec *s = action_spec_from_name(a.name);
		if (s != nullptr && s->type == spec->type)
			return &a;
	}
	return nullptr;
}

namespace {

std::string option(const BoundAction &a, const std::string &name) {
	for (const auto &[k, v] : a.options)
		if (k == name)
			return v;
	/* An alias's own: MaximizeHorz's direction. */
	if (name == "direction")
		if (const ActionSpec *s = action_spec_from_name(a.name); s && !s->direction.empty())
			return std::string(s->direction);
	return "";
}

std::string choice_label(const ActionInfo &info, const std::string &opt, const std::string &v) {
	for (const ActionOption &o : info.options)
		if (o.name == opt)
			for (const auto &[value, label] : o.choices)
				if (value == v)
					return label;
	return v;
}

std::string lower_first(std::string s) {
	if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z')
		s[0] = static_cast<char>(s[0] - 'A' + 'a');
	return s;
}

}  // namespace

std::string describe_action(const BoundAction &a) {
	if (a.advanced)
		return a.name + " (edit in rc.xml)";
	const ActionInfo *info = action_info(a.name);
	if (info == nullptr)
		return a.name;
	const std::string &n = info->name;
	if (n == "Execute") {
		std::string c = option(a, "command");
		return c.empty() ? info->label : "Run " + c;
	}
	if (n == "ShowMenu") {
		std::string m = option(a, "menu");
		if (m == "client-menu")
			return "Show the window menu";
		return m.empty() || m == "root-menu" ? "Show the root menu" : "Show the menu " + m;
	}
	if (n == "GoToDesktop" || n == "SendToDesktop") {
		std::string to = option(a, "to");
		std::string where = to.empty() ? "1" : to;
		if (to == "left" || to == "previous")
			where = "the previous";
		else if (to == "right" || to == "next")
			where = "the next";
		else if (to == "last")
			where = "the last shown";
		return (n == "GoToDesktop" ? "Go to " : "Send to ") +
				(where.find_first_not_of("0123456789") == std::string::npos
						? "desktop " + where : where + " desktop");
	}
	if (n == "SnapToEdge") {
		std::string d = option(a, "direction");
		return "Tile: " + lower_first(choice_label(*info, "direction", d.empty() ? "left" : d));
	}
	if (n == "MoveToEdge" || n == "GrowToEdge" || n == "ShrinkToEdge" ||
			n == "DirectionalTargetWindow") {
		std::string d = option(a, "direction");
		if (d.empty())
			return info->label;
		return info->label + ": " + lower_first(choice_label(*info, "direction", d));
	}
	if (n == "ToggleMaximize" || n == "Maximize" || n == "Unmaximize") {
		std::string d = option(a, "direction");
		if (d.empty() || d == "both")
			return info->label;
		return info->label + " (" + lower_first(choice_label(*info, "direction", d)) + ")";
	}
	return info->label;
}

std::string describe_actions(const std::vector<BoundAction> &actions) {
	std::string out;
	for (size_t i = 0; i < actions.size(); ++i) {
		std::string d = describe_action(actions[i]);
		out += i == 0 ? d : ", then " + lower_first(d);
	}
	return out;
}

}  // namespace wb
