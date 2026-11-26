#include "wb_test.hpp"

#include "waybox/settings_schema.hpp"
#include "waybox/wbconf.hpp"

#include <set>

using wb::SettingCategory;
using wb::SettingType;

WB_TEST(schema_merges_files_and_skips_bad_entries) {
	std::vector<SettingCategory> cats;
	std::string err;
	WB_CHECK(wb::merge_settings_schema(cats, R"({"categories":[
		{"id":"menu","title":"Menu","settings":[
			{"id":"a","title":"A","type":"bool","default":"yes","rc":{"path":"x/y","attr":"a"}},
			{"id":"nowhere","title":"No location","type":"int"},
			{"id":"custom","type":"custom","widget":"theme"}]}]})", &err));
	/* A second file: more settings in the same category, a duplicate id
	 * ignored, a new category. */
	WB_CHECK(wb::merge_settings_schema(cats, R"({"categories":[
		{"id":"menu","settings":[
			{"id":"a","title":"Again","type":"int","rc":{"path":"z"}},
			{"id":"b","title":"B","type":"enum","rc":{"path":"x/b"},
			 "choices":[{"value":"","label":"Default"},{"value":"on"}]}]},
		{"id":"plugin","title":"My Plugin","settings":[
			{"id":"p","title":"P","type":"lines","rc":{"path":"p/list","item":"i"}}]}]})"));
	WB_CHECK(cats.size() == 2);
	WB_CHECK(cats[0].settings.size() == 3);  /* a, custom, b */
	WB_CHECK(cats[0].settings[0].title == "A" && cats[0].settings[0].rc.attr == "a");
	WB_CHECK(cats[0].settings[1].type == SettingType::Custom);
	WB_CHECK(cats[0].settings[2].choices.size() == 2 &&
			cats[0].settings[2].choices[1].label == "on");
	WB_CHECK(cats[1].title == "My Plugin" && cats[1].settings[0].rc.item == "i");
	WB_CHECK(!wb::merge_settings_schema(cats, "{not json", &err) && !err.empty());
}

WB_TEST(search_matches_words_modified_and_ids) {
	wb::SettingDef s;
	s.id = "waybox.switcher.icons";
	s.title = "Show window icons";
	s.description = "In the Alt+Tab list.";
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "", false));
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "ICONS alt", false));
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "switcher", false));
	WB_CHECK(!wb::setting_matches(s, "Window Switcher", "icons font", false));
	WB_CHECK(!wb::setting_matches(s, "Window Switcher", "@modified", false));
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "@modified icons", true));
	WB_CHECK(!wb::setting_matches(s, "Window Switcher", "@problems", true));
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "@problems", false, true));
	WB_CHECK(!wb::setting_matches(s, "Window Switcher", "@problems nothing", false, true));
	WB_CHECK(wb::setting_matches(s, "Window Switcher", "@PROBLEMS icons", false, true));
	WB_CHECK(wb::setting_matches(s, "", "@id:switcher.ic", false));
	WB_CHECK(!wb::setting_matches(s, "", "@id:menu", false));
}

/* Every setting in the shipped schema can be read and written where it
 * says it lives (and removed again). */
WB_TEST(shipped_schema_round_trips_through_rc_xml) {
	std::vector<std::string> errors;
	auto cats = wb::load_settings_schema({WB_SOURCE_ROOT "/data/wbconf"}, &errors);
	WB_CHECK(errors.empty());
	WB_CHECK(cats.size() >= 9);
	std::set<std::string> ids;
	for (const SettingCategory &c : cats)
		for (const wb::SettingDef &s : c.settings) {
			WB_CHECK(ids.insert(s.id).second);
			if (s.type == SettingType::Custom)
				continue;
			auto doc = wb::RcDocument::from_string(
					"<?xml version=\"1.0\"?>\n"
					"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
					"</openbox_config>\n");
			std::string v = s.type == SettingType::Font ? "Serif Bold 12"
					: s.type == SettingType::Lines ? "one\ntwo & three"
					: !s.choices.empty() ? s.choices.back().value
					: s.type == SettingType::Int ? "7" : "a & b";
			WB_CHECK(!doc->get(s.rc));
			doc->set(s.rc, v);
			auto again = wb::RcDocument::from_string(doc->to_string());
			WB_CHECK(again->get(s.rc) == std::optional<std::string>(v));
			again->set(s.rc, std::nullopt);
			WB_CHECK(!again->get(s.rc));
			/* Removing it leaves no empty elements behind. */
			WB_CHECK(again->to_string().find("<waybox") == std::string::npos);
			WB_CHECK(again->to_string().find("<mouse") == std::string::npos);
		}
}

WB_TEST(rc_get_set_keeps_the_rest) {
	auto doc = wb::RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <waybox>\n"
			"    <!-- mine -->\n"
			"    <switcher osd=\"no\" icons=\"no\"/>\n"
			"  </waybox>\n"
			"</openbox_config>\n");
	wb::RcLocation icons{"waybox/switcher", "icons", "", ""};
	wb::RcLocation osd{"waybox/switcher", "osd", "", ""};
	WB_CHECK(doc->get(icons) == std::optional<std::string>("no"));
	doc->set(icons, std::nullopt);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<switcher osd=\"no\"/>") != std::string::npos);
	doc->set(osd, std::nullopt);
	out = doc->to_string();
	/* The emptied <switcher> goes; <waybox> stays for its comment. */
	WB_CHECK(out.find("switcher") == std::string::npos);
	WB_CHECK(out.find("<!-- mine -->") != std::string::npos);
}

WB_TEST(shipped_schema_groups_the_margins) {
	auto cats = wb::load_settings_schema({WB_SOURCE_ROOT "/data/wbconf"});
	int in_group = 0;
	for (const SettingCategory &c : cats)
		for (const wb::SettingDef &s : c.settings)
			if (s.group == "Reserved margins")
				++in_group;
	WB_CHECK(in_group == 4);
	/* Integrations stays (wbconf adds the programs) even with few settings. */
	bool integrations = false;
	for (const SettingCategory &c : cats)
		integrations = integrations || (c.id == "integrations" && !c.description.empty());
	WB_CHECK(integrations);
}
