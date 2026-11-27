/*
 * Pure per-application rule matching: the standard library and libc's
 * POSIX regex (for match="regex"), so it links into the standalone unit
 * test.
 */
#include "waybox/applications.hpp"

#include <regex.h>

namespace wb {

namespace {

/* A pattern of `kind` against `text`; a regex that doesn't compile, never.
 * (Compiled each time: rules are few and windows open rarely.) */
bool pattern_match(AppRule::Patterns kind, const std::string &pattern, std::string_view text) {
	if (kind == AppRule::Patterns::Glob)
		return glob_match(pattern, text);
	regex_t re;
	int flags = REG_EXTENDED | REG_NOSUB | (kind == AppRule::Patterns::IRegex ? REG_ICASE : 0);
	if (regcomp(&re, pattern.c_str(), flags) != 0)
		return false;
	bool hit = regexec(&re, std::string(text).c_str(), 0, nullptr, 0) == 0;
	regfree(&re);
	return hit;
}

}  // namespace

std::string app_rule_pattern_error(const AppRule &rule) {
	if (rule.patterns == AppRule::Patterns::Glob)
		return "";
	int flags = REG_EXTENDED | REG_NOSUB |
			(rule.patterns == AppRule::Patterns::IRegex ? REG_ICASE : 0);
	for (const auto &[what, p] : {std::pair<const char *, const std::string *>{"class",
			&rule.class_pattern}, {"name", &rule.name_pattern}, {"title", &rule.title_pattern},
			{"tag", &rule.tag_pattern}}) {
		if (p->empty())
			continue;
		regex_t re;
		int err = regcomp(&re, p->c_str(), flags);
		if (err == 0) {
			regfree(&re);
			continue;
		}
		char msg[256];
		regerror(err, &re, msg, sizeof msg);
		return std::string(what) + ": " + msg;
	}
	return "";
}

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
		auto k = rule.patterns;
		if (!rule.class_pattern.empty() && !pattern_match(k, rule.class_pattern, app_id))
			continue;
		if (!rule.name_pattern.empty() && !pattern_match(k, rule.name_pattern, app_id))
			continue;
		if (!rule.title_pattern.empty() && !pattern_match(k, rule.title_pattern, title))
			continue;
		/* A tag pattern never matches a window without a tag. */
		if (!rule.tag_pattern.empty() && (tag.empty() || !pattern_match(k, rule.tag_pattern, tag)))
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
