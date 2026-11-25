/*
 * Pure half of the action framework: the registry and parsing.
 *
 * This translation unit deliberately depends on nothing but the C++ standard
 * library so it can be linked into the standalone unit test (test/action_test)
 * without pulling in wlroots. The live dispatch (run_action) lives in seat.cpp.
 */
#include "waybox/action.hpp"

#include <algorithm>
#include <array>

namespace wb {

namespace {

/* The single source of truth for known actions. Adding a row here is all that
 * is needed to register a new action name for parsing; wire its behaviour into
 * run_action() to make it do something. */
constexpr std::array<ActionSpec, 37> kRegistry = {{
	{"Execute",        ActionType::Execute,        true},
	{"NextWindow",     ActionType::NextWindow,     false},
	{"PreviousWindow", ActionType::PreviousWindow, false},
	{"Close",          ActionType::Close,          false},
	{"Focus",          ActionType::Focus,          false},
	{"ToggleMaximize", ActionType::ToggleMaximize, false},
	/* Openbox's maximize family: each takes <direction>; the Full, Horz and
	 * Vert names fix it. ToggleMaximizeHorizontal/Vertical: Waybox's earlier
	 * names, kept. */
	{"ToggleMaximizeFull", ActionType::ToggleMaximize, false, "both"},
	{"ToggleMaximizeHorz", ActionType::ToggleMaximize, false, "horizontal"},
	{"ToggleMaximizeVert", ActionType::ToggleMaximize, false, "vertical"},
	{"ToggleMaximizeHorizontal", ActionType::ToggleMaximize, false, "horizontal"},
	{"ToggleMaximizeVertical",   ActionType::ToggleMaximize, false, "vertical"},
	{"Maximize",       ActionType::Maximize,       false},
	{"MaximizeFull",   ActionType::Maximize,       false, "both"},
	{"MaximizeHorz",   ActionType::Maximize,       false, "horizontal"},
	{"MaximizeVert",   ActionType::Maximize,       false, "vertical"},
	{"Unmaximize",     ActionType::Unmaximize,     false},
	{"UnmaximizeFull", ActionType::Unmaximize,     false, "both"},
	{"UnmaximizeHorz", ActionType::Unmaximize,     false, "horizontal"},
	{"UnmaximizeVert", ActionType::Unmaximize,     false, "vertical"},
	/* Openbox's name; Fullscreen is Waybox's earlier one, kept. */
	{"ToggleFullscreen", ActionType::Fullscreen,   false},
	{"Fullscreen",     ActionType::Fullscreen,     false},
	{"Iconify",        ActionType::Iconify,        false},
	{"Shade",          ActionType::Shade,          false},
	{"Unshade",        ActionType::Unshade,        false},
	{"ToggleShade",    ActionType::ToggleShade,    false},
	{"Raise",          ActionType::Raise,          false},
	{"Lower",          ActionType::Lower,          false},
	{"Exit",           ActionType::Exit,           false},
	{"Reconfigure",    ActionType::Reconfigure,    false},
	{"ToggleDecorations", ActionType::ToggleDecorations, false},
	{"ShowMenu",       ActionType::ShowMenu,       true},
	{"GoToDesktop",    ActionType::GoToDesktop,    false},
	{"SendToDesktop",  ActionType::SendToDesktop,  false},
	{"ToggleOmnipresent", ActionType::ToggleOmnipresent, false},
	{"SnapToEdge",     ActionType::SnapToEdge,     false},
	{"Move",           ActionType::Move,           false},
	{"Resize",         ActionType::Resize,         false},
}};

}  // namespace

std::span<const ActionSpec> action_registry() {
	return kRegistry;
}

const ActionSpec *action_spec_from_name(std::string_view name) {
	auto it = std::find_if(kRegistry.begin(), kRegistry.end(),
			[name](const ActionSpec &spec) { return spec.name == name; });
	return it != kRegistry.end() ? &*it : nullptr;
}

std::optional<Action> make_action(std::string_view name, std::string_view command) {
	const ActionSpec *spec = action_spec_from_name(name);
	if (spec == nullptr)
		return std::nullopt;

	Action action{spec->type};
	if (spec->takes_command)
		action.command = std::string(command);
	action.direction = std::string(spec->direction);
	return action;
}

}  // namespace wb
