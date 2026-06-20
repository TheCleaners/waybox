#ifndef WB_APPLICATIONS_HPP
#define WB_APPLICATIONS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * Per-application rules (rc.xml <applications>). A rule matches a window by
 * glob patterns on its identifiers and, when it matches, overrides chosen
 * aspects of how the window is mapped (position/size/maximized/...). Matching
 * is pure and unit-tested; the overrides are applied in the map handler.
 *
 * Wayland xdg-shell exposes app_id and title (there is no X11 window role), so
 * Openbox's class/name both match against app_id and title matches the title.
 */

struct AppRule {
	/* Glob patterns ("*"/"?"); an empty pattern means "match anything". */
	std::string class_pattern;  /* vs app_id (Openbox class) */
	std::string name_pattern;   /* vs app_id (Openbox name) */
	std::string title_pattern;  /* vs title */

	/* Overrides; std::nullopt leaves that aspect to the default behaviour. */
	std::optional<bool> maximized;
	std::optional<bool> fullscreen;
	std::optional<bool> iconic;      /* start minimised */
	std::optional<bool> focus;       /* whether to focus on map */
	std::optional<bool> decor;       /* server-side decorations on/off */
	std::optional<int> x;
	std::optional<int> y;
	std::optional<int> width;
	std::optional<int> height;
};

/* Glob match supporting '*' (any run) and '?' (any one char). Case-sensitive. */
bool glob_match(std::string_view pattern, std::string_view text);

/* The first rule whose every non-empty pattern matches, or nullptr. */
const AppRule *match_app_rule(const std::vector<AppRule> &rules,
		std::string_view app_id, std::string_view title);

}  // namespace wb

#endif /* WB_APPLICATIONS_HPP */
