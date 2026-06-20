#ifndef WB_ACTION_HPP
#define WB_ACTION_HPP

#include <optional>
#include <span>
#include <string>
#include <string_view>

struct wb_server;

namespace wb {

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
	ToggleMaximizeHorizontal,
	ToggleMaximizeVertical,
	Fullscreen,
	Iconify,
	Shade,
	Unshade,
	Raise,
	Lower,
	Exit,
	Reconfigure,
	ToggleDecorations,
	ShowMenu,
};

/* A parsed action ready to run. `command` is only meaningful for actions whose
 * spec has takes_command == true (currently only Execute). */
struct Action {
	ActionType type;
	std::string command;
};

/* Registry entry describing one action by its rc.xml name. */
struct ActionSpec {
	std::string_view name;
	ActionType type;
	bool takes_command;
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

}  // namespace wb

#endif /* WB_ACTION_HPP */
