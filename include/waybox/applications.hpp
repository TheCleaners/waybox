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
	/* How its patterns are read: globs ("*"/"?", the whole text; Openbox's),
	 * or, opt-in with match="regex" / "iregex", POSIX extended regular
	 * expressions as egrep reads them (anywhere in the text unless anchored
	 * with ^...$; iregex ignoring case). */
	enum class Patterns { Glob, Regex, IRegex };
	Patterns patterns = Patterns::Glob;
	/* The patterns; an empty pattern means "match anything". */
	std::string class_pattern;  /* vs app_id (Openbox class) */
	std::string name_pattern;   /* vs app_id (Openbox name) */
	std::string title_pattern;  /* vs title */
	std::string tag_pattern;    /* vs the window's tag (xdg-toplevel-tag) */

	/* Overrides; std::nullopt leaves that aspect to the default behaviour. */
	std::optional<bool> maximized;
	std::optional<bool> fullscreen;
	std::optional<bool> iconic;      /* start minimised */
	std::optional<bool> focus;       /* whether to focus on map */
	std::optional<bool> decor;       /* server-side decorations on/off */
	std::optional<int> layer;        /* <layer>: -1 below, 0 normal, 1 above */
	/* Desktop to open on (0-based), or kAllDesktops for every desktop. */
	static constexpr int kAllDesktops = -1;
	std::optional<int> desktop;
	std::optional<int> x;
	std::optional<int> y;
	std::optional<int> width;
	std::optional<int> height;
	bool operator==(const AppRule &) const = default;
};

/* A rule in words, for wbconf's list: what it does, each its own phrase
 * ("Full screen", "On desktop 2", "900 × 600", "At 200, 120"), in a fixed
 * order; empty when it sets nothing. */
std::vector<std::string> app_rule_effects(const AppRule &rule);

/* For a regex rule, why a pattern doesn't compile ("title: unmatched
 * parentheses"); empty when they all do, and for globs. Such a pattern
 * never matches. */
std::string app_rule_pattern_error(const AppRule &rule);

/* Glob match supporting '*' (any run) and '?' (any one char). Case-sensitive. */
bool glob_match(std::string_view pattern, std::string_view text);

/* The first rule whose every non-empty pattern matches, or nullptr. */
const AppRule *match_app_rule(const std::vector<AppRule> &rules,
		std::string_view app_id, std::string_view title, std::string_view tag = {});

}  // namespace wb

#endif /* WB_APPLICATIONS_HPP */
