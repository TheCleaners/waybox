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

WB_TEST(rules_match_window_tags) {
	std::vector<wb::AppRule> rules(2);
	rules[0].class_pattern = "firefox";
	rules[0].tag_pattern = "pip*";
	rules[1].class_pattern = "firefox";
	/* The tagged window gets the tag rule; others fall through. */
	WB_CHECK(match_app_rule(rules, "firefox", "Picture-in-Picture", "pip") == &rules[0]);
	WB_CHECK(match_app_rule(rules, "firefox", "Mozilla Firefox", "main") == &rules[1]);
	/* No tag: a tag rule never matches. */
	WB_CHECK(match_app_rule(rules, "firefox", "x") == &rules[1]);
	WB_CHECK(match_app_rule(rules, "firefox", "x", "") == &rules[1]);
}

WB_TEST(app_rule_in_words) {
	using V = std::vector<std::string>;
	AppRule r;
	WB_CHECK(app_rule_effects(r).empty());
	r.fullscreen = true;
	r.desktop = AppRule::kAllDesktops;
	r.x = 200;
	r.y = 120;
	r.width = 900;
	r.height = 600;
	r.decor = false;
	WB_CHECK(app_rule_effects(r) == (V{"On every desktop", "Full screen", "At 200, 120",
			"900 × 600", "No title bar"}));
	AppRule s;
	s.desktop = 1;
	s.iconic = true;
	s.layer = 1;
	s.focus = false;
	s.width = 400;
	WB_CHECK(app_rule_effects(s) == (V{"On desktop 2", "Starts minimized", "400 × auto",
			"Always on top", "Not focused when it opens"}));
}

WB_TEST(regex_patterns) {
	using P = AppRule::Patterns;
	auto rule = [](P kind, std::string cls, std::string title = "") {
		AppRule r;
		r.patterns = kind;
		r.class_pattern = std::move(cls);
		r.title_pattern = std::move(title);
		return r;
	};
	/* As egrep: anywhere in the text, unless anchored. */
	std::vector<AppRule> rules = {rule(P::Regex, "^(foot|alacritty)$")};
	WB_CHECK(wb::match_app_rule(rules, "foot", "") == &rules[0]);
	WB_CHECK(wb::match_app_rule(rules, "alacritty", "") == &rules[0]);
	WB_CHECK(wb::match_app_rule(rules, "footclient", "") == nullptr);
	rules = {rule(P::Regex, "", "scratch|notes")};
	WB_CHECK(wb::match_app_rule(rules, "foot", "my notes") == &rules[0]);
	WB_CHECK(wb::match_app_rule(rules, "foot", "Notes") == nullptr);  /* case matters */
	rules = {rule(P::IRegex, "", "notes")};
	WB_CHECK(wb::match_app_rule(rules, "foot", "Notes") == &rules[0]);
	/* A glob's "*" is a regex's "any run of the char before": not the same. */
	rules = {rule(P::Glob, "foo*")};
	WB_CHECK(wb::match_app_rule(rules, "football", "") == &rules[0]);
	WB_CHECK(wb::app_rule_pattern_error(rules[0]).empty());
	/* Doesn't compile: never matches, and says why. */
	AppRule bad = rule(P::Regex, "foot", "(unclosed");
	rules = {bad};
	WB_CHECK(wb::match_app_rule(rules, "foot", "(unclosed") == nullptr);
	WB_CHECK(wb::app_rule_pattern_error(bad).rfind("title: ", 0) == 0);
	WB_CHECK(wb::app_rule_pattern_error(rule(P::Regex, "a+b")).empty());
}

WB_TEST(openbox_positions) {
	/* An area from 100, 1000 long; a window 200 long. */
	WB_CHECK(wb::resolve_position("50", 100, 1000, 200) == 150);
	WB_CHECK(wb::resolve_position("-50", 100, 1000, 200) == 850);   /* far edge 50 from the end */
	WB_CHECK(wb::resolve_position("center", 100, 1000, 200) == 500);
	WB_CHECK(wb::resolve_position("25%", 100, 1000, 200) == 350);
	WB_CHECK(wb::resolve_position("-10%", 100, 1000, 200) == 800);
	WB_CHECK(wb::resolve_position("0", 100, 1000, 200) == 100);
	WB_CHECK(!wb::resolve_position("left", 100, 1000, 200));
	WB_CHECK(!wb::resolve_position("", 100, 1000, 200));
	WB_CHECK(!wb::resolve_position("5px", 100, 1000, 200));
	WB_CHECK(wb::position_spec_ok("-3%") && !wb::position_spec_ok("%"));
	AppRule r;
	r.x_spec = "center";
	r.y = 40;
	r.monitor = "2";
	WB_CHECK(wb::app_rule_effects(r) == (std::vector<std::string>{"At center, 40", "On monitor 2"}));
}

WB_TEST(rule_monitor_by_place_or_name) {
	/* Plugged in as DP-2 (right), HDMI-A-1 (left), DP-1 (below the left). */
	std::vector<wb::MonitorPlace> m = {{"DP-2", 1920, 0}, {"HDMI-A-1", 0, 0}, {"DP-1", 0, 1080}};
	WB_CHECK(wb::rule_monitor(m, "1") == size_t{1});   /* leftmost, top */
	WB_CHECK(wb::rule_monitor(m, "2") == size_t{2});   /* left, below */
	WB_CHECK(wb::rule_monitor(m, "3") == size_t{0});   /* right */
	WB_CHECK(wb::rule_monitor(m, "primary") == size_t{1});
	WB_CHECK(wb::rule_monitor(m, "DP-1") == size_t{2});
	WB_CHECK(!wb::rule_monitor(m, "4") && !wb::rule_monitor(m, "0") && !wb::rule_monitor(m, "VGA-1"));
	WB_CHECK(!wb::rule_monitor(m, "mouse"));
	AppRule r;
	r.monitor = "DP-1";
	WB_CHECK(wb::app_rule_effects(r) == (std::vector<std::string>{"On DP-1"}));
}
