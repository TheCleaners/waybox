#ifndef WB_CONDITIONS_HPP
#define WB_CONDITIONS_HPP

#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/action.hpp"

namespace wb {

/*
 * Openbox's conditional actions: If, ForEach and Stop (rc.xml and menu.xml).
 *
 *   <action name="If">
 *     <title type="regex">- Firefox$</title><maximized>no</maximized>
 *     <then><action name="Maximize"/></then>
 *     <else><action name="Unmaximize"/></else>
 *   </action>
 *
 * The conditions can sit directly in <If> (one query, on the window the
 * action is for), or in <query> elements, all of which must hold; a query
 * with target="focus" asks about the focused window. ForEach runs the same
 * test on every window (<then>/<else> for each, <none> if none matched);
 * Stop ends the actions after it in its list, and a ForEach loop.
 *
 * Matching (query_true) is pure, on facts about a window, so it's unit
 * tested; running the branches is in the seat code.
 */

/* A text condition: <title>, <class>, <name>, <role>, <type>, with
 * type="pattern" (a glob with * and ?, the default), "regex" (found
 * anywhere in the text, as Openbox), or "exact". */
struct TextMatch {
	enum class Kind { Glob, Regex, Exact };
	Kind kind = Kind::Glob;
	std::string pattern;
	bool set = false;
	std::shared_ptr<const std::regex> regex;  /* compiled; null if invalid */
};

struct Query {
	bool focus_target = false;  /* target="focus" */
	std::optional<bool> shaded, maximized, maximized_horizontal, maximized_vertical,
			iconified, fullscreen, focused, focusable, urgent, undecorated, omnipresent;
	enum class Desktop { Any, Number, Current, Other, Last };
	Desktop desktop = Desktop::Any;
	int desktop_number = 0;  /* 1-based, for Desktop::Number */
	int active_desktop = 0;  /* <activedesktop>: 1-based, 0 = not asked */
	TextMatch title, cls, name, role, type;
	int monitor = 0;  /* 1-based, 0 = not asked */
};

struct Conditional {
	std::vector<Query> queries;
	std::vector<Action> then_actions, else_actions, none_actions;
};

/* What a query can ask about one window. */
struct WindowFacts {
	std::string title, app_id, tag;
	std::string type = "normal";  /* "dialog" for a window with a parent */
	bool shaded = false, maximized_horizontal = false, maximized_vertical = false;
	bool iconified = false, fullscreen = false, focused = false, focusable = true;
	bool urgent = false, decorated = true, omnipresent = false;
	int desktop = 0;   /* 0-based */
	int monitor = 0;   /* 1-based; 0 = none */
};

struct DesktopFacts {
	int current = 0;  /* 0-based */
	int last = 0;
};

bool text_matches(const TextMatch &m, std::string_view text);

/* Whether every query holds. `action_target` and `focus_target` may be
 * null (no window): then a query is false, unless it asked about the
 * desktop shown (<activedesktop>) and that held, as in Openbox. */
bool conditions_true(const std::vector<Query> &queries, const WindowFacts *action_target,
		const WindowFacts *focus_target, const DesktopFacts &desktops);

/* Make a TextMatch from a kind ("pattern"/"" , "regex", "exact") and a
 * pattern; an invalid regex matches nothing. */
TextMatch make_text_match(std::string_view kind, std::string pattern);

}  // namespace wb

#endif
