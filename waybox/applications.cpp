/*
 * Pure per-application rule matching. Standard-library only, so it links into
 * the standalone unit test.
 */
#include "waybox/applications.hpp"

namespace wb {

bool glob_match(std::string_view pattern, std::string_view text) {
	std::size_t p = 0;
	std::size_t t = 0;
	std::size_t star = std::string_view::npos;
	std::size_t star_t = 0;

	while (t < text.size()) {
		if (p < pattern.size() &&
				(pattern[p] == '?' || pattern[p] == text[t])) {
			++p;
			++t;
		} else if (p < pattern.size() && pattern[p] == '*') {
			star = p++;       /* remember the '*' and retry it on backtrack */
			star_t = t;
		} else if (star != std::string_view::npos) {
			p = star + 1;     /* let the last '*' absorb one more char */
			t = ++star_t;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*')
		++p;
	return p == pattern.size();
}

const AppRule *match_app_rule(const std::vector<AppRule> &rules,
		std::string_view app_id, std::string_view title, std::string_view tag) {
	for (const AppRule &rule : rules) {
		if (!rule.class_pattern.empty() && !glob_match(rule.class_pattern, app_id))
			continue;
		if (!rule.name_pattern.empty() && !glob_match(rule.name_pattern, app_id))
			continue;
		if (!rule.title_pattern.empty() && !glob_match(rule.title_pattern, title))
			continue;
		/* A tag pattern never matches a window without a tag. */
		if (!rule.tag_pattern.empty() && (tag.empty() || !glob_match(rule.tag_pattern, tag)))
			continue;
		/* A rule with no patterns at all matches everything; that is the
		 * caller's choice (Openbox treats it the same way). */
		return &rule;
	}
	return nullptr;
}

std::vector<std::string> app_rule_effects(const AppRule &r) {
	std::vector<std::string> out;
	auto num = [](int n) { return std::to_string(n); };
	if (r.desktop)
		out.push_back(*r.desktop == AppRule::kAllDesktops ? "On every desktop"
				: "On desktop " + num(*r.desktop + 1));
	if (r.fullscreen)
		out.push_back(*r.fullscreen ? "Full screen" : "Not full screen");
	if (r.maximized)
		out.push_back(*r.maximized ? "Maximized" : "Not maximized");
	if (r.iconic)
		out.push_back(*r.iconic ? "Starts minimized" : "Not minimized");
	if (r.x || r.y)
		out.push_back("At " + (r.x ? num(*r.x) : std::string("auto")) + ", " +
				(r.y ? num(*r.y) : std::string("auto")));
	if (r.width || r.height)
		out.push_back((r.width ? num(*r.width) : std::string("auto")) + " \u00d7 " +
				(r.height ? num(*r.height) : std::string("auto")));
	if (r.decor)
		out.push_back(*r.decor ? "With title bar" : "No title bar");
	if (r.layer)
		out.push_back(*r.layer > 0 ? "Always on top" : *r.layer < 0 ? "Always below" : "Normal layer");
	if (r.focus)
		out.push_back(*r.focus ? "Focused when it opens" : "Not focused when it opens");
	return out;
}

}  // namespace wb
