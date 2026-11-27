#ifndef WB_ACTION_HPP
#define WB_ACTION_HPP

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wb_server;

namespace wb {

struct Conditional;

/*
 * Action framework.
 *
 * rc.xml binds keys (and, later, mouse buttons and menu items) to an ordered
 * list of named actions. This header models that as a small registry of known
 * actions plus a parsed, ready-to-run Action value.
 *
 * The registry and parsing (action_registry/action_spec_from_name/make_action)
 * are pure and unit-tested in test/action_test.cpp; run_action() drives live
 * wlroots/Wayland state and is defined alongside the seat code.
 */

enum class ActionType {
	Execute,
	NextWindow,
	PreviousWindow,
	Close,
	ToggleMaximize,
	Maximize,       /* <direction>: both (default), horizontal, vertical */
	Unmaximize,
	Fullscreen,
	Iconify,
	Shade,
	Unshade,
	ToggleShade,
	Raise,
	Lower,
	Exit,
	Reconfigure,
	ToggleDecorations,
	ShowMenu,
	GoToDesktop,
	SendToDesktop,
	ToggleOmnipresent,
	/* Openbox's layers: <direction> (or the name) top, normal or bottom;
	 * the toggles between that layer and normal. */
	SendToLayer,
	ToggleAlwaysOnTop,
	ToggleAlwaysOnBottom,
	ShowAbout,  /* Waybox's: the About card */
	/* Openbox's geometry actions (geometry_actions.hpp): their options in
	 * `args` (<x>, <y>, <width>, <height>, <left>, ...), the edge ones'
	 * <direction> (or the name's). */
	MoveResizeTo,
	MoveRelative,
	ResizeRelative,
	MoveToCenter,
	MoveToEdge,
	GrowToEdge,
	ShrinkToEdge,
	GrowToFill,
	SnapToEdge,
	Move,
	Resize,
	Focus,
	If,       /* Openbox's conditional actions (conditions.hpp) */
	ForEach,
	Stop,
	/* Not an rc.xml action: calls a function a script gave a menu entry
	 * (script_callback; see wb.show_menu in waybox-lua(5)). */
	ScriptCall,
};

/* A parsed action ready to run. `command` is only meaningful for actions whose
 * spec has takes_command == true (Execute, ShowMenu). `to`, `wrap` and
 * `follow` are GoToDesktop/SendToDesktop's options (<to>, <wrap>, <follow>). */
struct Action {
	ActionType type;
	std::string command{};
	std::string to{};
	bool wrap = true;
	bool follow = true;
	std::string direction{};  /* SnapToEdge: left, right, up, down, up-left, ... */
	/* Other options by name, as given (the geometry actions'). */
	std::map<std::string, std::string> args{};
	int script_callback = 0;  /* ScriptCall */
	/* If and ForEach: the conditions and the actions of each branch. */
	std::shared_ptr<const Conditional> conditional{};
};

/* Registry entry describing one action by its rc.xml name. */
struct ActionSpec {
	std::string_view name;
	ActionType type;
	bool takes_command;
	/* A direction the name itself implies (Openbox's MaximizeHorz is
	 * Maximize with <direction>horizontal</direction>); empty: none. */
	std::string_view direction = {};
};

/* The full table of known actions, in declaration order. */
std::span<const ActionSpec> action_registry();

/* Look up an action by its rc.xml name (case-sensitive, matching Openbox).
 * Returns nullptr when the name is not registered. */
const ActionSpec *action_spec_from_name(std::string_view name);

/* Build an Action from a name and an (optional) command string. Returns
 * std::nullopt when the name is not a registered action. The command is
 * ignored for actions that do not take one. */
std::optional<Action> make_action(std::string_view name, std::string_view command);

/* Run a single parsed action against the server. Defined in seat.cpp; not
 * unit-tested because it drives live compositor state. */
void run_action(const Action &action, struct wb_server *server);
/* Run a list of actions in order: the actions after a Stop don't run (nor
 * after a Reconfigure: they belong to the old configuration). True if it
 * ended early that way. */
bool run_actions(const std::vector<Action> &actions, struct wb_server *server);

}  // namespace wb

#endif /* WB_ACTION_HPP */
