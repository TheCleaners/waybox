#include "wb_test.hpp"

#include "waybox/applications.hpp"

#include <vector>

using wb::AppRule;
using wb::glob_match;
using wb::match_app_rule;

WB_TEST(glob_literal_and_wildcards) {
	WB_CHECK(glob_match("foot", "foot"));
	WB_CHECK(!glob_match("foot", "Foot"));   /* case sensitive */
	WB_CHECK(!glob_match("foot", "footx"));
	WB_CHECK(glob_match("*", "anything"));
	WB_CHECK(glob_match("*", ""));
	WB_CHECK(glob_match("foo*", "foobar"));
	WB_CHECK(glob_match("*bar", "foobar"));
	WB_CHECK(glob_match("*oo*", "foobar"));
	WB_CHECK(glob_match("f??t", "foot"));
	WB_CHECK(!glob_match("f??t", "ft"));
	WB_CHECK(glob_match("a*b*c", "axxbyyc"));
	WB_CHECK(!glob_match("a*b*c", "axxbyy"));
}

WB_TEST(glob_edge_cases) {
	WB_CHECK(glob_match("", ""));
	WB_CHECK(!glob_match("", "x"));
	WB_CHECK(glob_match("**", "abc"));  /* consecutive stars */
	WB_CHECK(glob_match("*a", "a"));
}

WB_TEST(rule_matches_on_class) {
	std::vector<AppRule> rules(1);
	rules[0].class_pattern = "firefox*";
	WB_CHECK(match_app_rule(rules, "firefox-esr", "Mozilla") == &rules[0]);
	WB_CHECK(match_app_rule(rules, "chromium", "x") == nullptr);
}

WB_TEST(rule_requires_all_patterns) {
	std::vector<AppRule> rules(1);
	rules[0].class_pattern = "foot";
	rules[0].title_pattern = "*vim*";
	/* both must match */
	WB_CHECK(match_app_rule(rules, "foot", "nvim - file") == &rules[0]);
	WB_CHECK(match_app_rule(rules, "foot", "a shell") == nullptr);
	WB_CHECK(match_app_rule(rules, "alacritty", "nvim") == nullptr);
}

WB_TEST(rule_first_match_wins) {
	std::vector<AppRule> rules(2);
	rules[0].class_pattern = "foot";
	rules[1].class_pattern = "*";  /* catch-all */
	WB_CHECK(match_app_rule(rules, "foot", "") == &rules[0]);
	WB_CHECK(match_app_rule(rules, "other", "") == &rules[1]);
}

WB_TEST(empty_patterns_match_anything) {
	std::vector<AppRule> rules(1);  /* all patterns empty */
	WB_CHECK(match_app_rule(rules, "anything", "any title") == &rules[0]);
}
