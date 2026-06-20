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
		std::string_view app_id, std::string_view title) {
	for (const AppRule &rule : rules) {
		if (!rule.class_pattern.empty() && !glob_match(rule.class_pattern, app_id))
			continue;
		if (!rule.name_pattern.empty() && !glob_match(rule.name_pattern, app_id))
			continue;
		if (!rule.title_pattern.empty() && !glob_match(rule.title_pattern, title))
			continue;
		/* A rule with no patterns at all matches everything; that is the
		 * caller's choice (Openbox treats it the same way). */
		return &rule;
	}
	return nullptr;
}

}  // namespace wb
