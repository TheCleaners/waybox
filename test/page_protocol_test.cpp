#include "wb_test.hpp"

#include "waybox/page_protocol.hpp"

#include <string>

using wb::json::Value;
using wb::page::Kind;

namespace {

wb::json::Value js(const char *text) {
	auto v = wb::json::parse(text);
	return v ? *v : Value();
}

const char *kPage = R"({"controls": [
  {"kind": "group", "label": "Where", "children": [
    {"kind": "choice", "key": "anchor", "label": "Corner", "value": "top-right",
     "choices": ["top-left", "top-right"]}]},
  {"kind": "number", "key": "timeout", "label": "Disappear after", "unit": "ms",
   "min": 0, "max": 60000, "value": 5000},
  {"kind": "switch", "key": "icons", "label": "Icons", "value": true},
  {"kind": "label", "label": "Some text"},
  {"kind": "button", "key": "test", "label": "Send a test"}]})";

}  // namespace

WB_TEST(page_round_trips) {
	std::string err;
	auto p = wb::page::page_from_json(js(kPage), &err);
	WB_CHECK(p.has_value() && err.empty());
	WB_CHECK(p->controls.size() == 5 && p->controls[0].kind == Kind::Group);
	WB_CHECK(p->controls[0].children.size() == 1 &&
			p->controls[0].children[0].choices.size() == 2);
	/* Values as text: a number as written, a switch "on". */
	WB_CHECK(p->controls[1].value == "5000" && p->controls[1].min == 0.0 &&
			p->controls[1].max == 60000.0 && p->controls[1].unit == "ms");
	WB_CHECK(p->controls[2].value == "on");
	auto again = wb::page::page_from_json(wb::page::page_to_json(*p));
	WB_CHECK(again && *again == *p);
	auto values = wb::page::page_values(*p);
	WB_CHECK(values.size() == 3 && values[0].first == "anchor" && values[0].second == "top-right");
}

WB_TEST(pages_that_cant_be_drawn) {
	std::string err;
	WB_CHECK(!wb::page::page_from_json(js(R"({"nope": []})"), &err) &&
			err.find("controls") != std::string::npos);
	WB_CHECK(!wb::page::page_from_json(js(R"({"controls": [{"kind": "slider", "key": "x"}]})"),
			&err) && err.find("slider") != std::string::npos);
	WB_CHECK(!wb::page::page_from_json(js(R"({"controls": [{"kind": "text"}]})"), &err) &&
			err.find("without a key") != std::string::npos);
	WB_CHECK(!wb::page::page_from_json(js(R"({"controls": [{"kind": "text", "key": "a"},
			{"kind": "group", "children": [{"kind": "switch", "key": "a"}]}]})"), &err) &&
			err.find("two controls") != std::string::npos);
	WB_CHECK(!wb::page::page_from_json(js(R"({"controls": [{"kind": "choice", "key": "c"}]})"),
			&err) && err.find("nothing to choose") != std::string::npos);
	/* Deep groups. */
	std::string deep = R"({"controls": [)";
	for (int i = 0; i < 6; ++i)
		deep += R"({"kind": "group", "children": [)";
	deep += R"({"kind": "label", "label": "x"})";
	for (int i = 0; i < 6; ++i)
		deep += "]}";
	deep += "]}";
	WB_CHECK(!wb::page::page_from_json(js(deep.c_str()), &err) &&
			err.find("4 deep") != std::string::npos);
	/* Labels and groups need no key. */
	WB_CHECK(wb::page::page_from_json(js(R"({"controls": [{"kind": "label", "label": "x"}]})")));
}

WB_TEST(updates_apply_to_the_control) {
	auto p = *wb::page::page_from_json(js(kPage));
	WB_CHECK(wb::page::apply_update(p, "timeout",
			js(R"({"value": 1000, "problem": "too short", "enabled": false})")));
	auto *c = wb::page::find_control(p, "timeout");
	WB_CHECK(c && c->value == "1000" && c->problem == "too short" && !c->enabled);
	WB_CHECK(wb::page::apply_update(p, "anchor", js(R"({"visible": false})")));
	WB_CHECK(!wb::page::find_control(p, "anchor")->visible);
	/* problem: null clears it. */
	WB_CHECK(wb::page::apply_update(p, "timeout", js(R"({"problem": null})")));
	WB_CHECK(c->problem.empty());
	WB_CHECK(!wb::page::apply_update(p, "nothing", js(R"({"value": 1})")));
	/* A button has no value. */
	WB_CHECK(wb::page::apply_update(p, "test", js(R"({"value": "x", "label": "Again"})")));
	WB_CHECK(wb::page::find_control(p, "test")->value.empty() &&
			wb::page::find_control(p, "test")->label == "Again");
}

WB_TEST(what_a_page_may_touch) {
	std::vector<std::string> files = {"~/.config/mako/config", "/etc/xdg/mako/config"};
	WB_CHECK(wb::page::path_allowed("~/.config/mako/config", files, "/home/u"));
	WB_CHECK(wb::page::path_allowed("/home/u/.config/mako/config", files, "/home/u"));
	WB_CHECK(wb::page::path_allowed("/etc/xdg/mako/config", files, "/home/u"));
	WB_CHECK(!wb::page::path_allowed("~/.config/mako/other", files, "/home/u"));
	WB_CHECK(!wb::page::path_allowed("~/.config/mako/../mako/config", files, "/home/u"));
	WB_CHECK(!wb::page::path_allowed("config", files, "/home/u"));
	WB_CHECK(!wb::page::path_allowed("", files, "/home/u"));
	std::vector<std::string> runs = {"makoctl", "notify-send"};
	WB_CHECK(wb::page::run_allowed({"makoctl", "reload"}, runs));
	WB_CHECK(!wb::page::run_allowed({"/usr/bin/makoctl", "reload"}, runs));
	WB_CHECK(!wb::page::run_allowed({"sh", "-c", "makoctl"}, runs));
	WB_CHECK(!wb::page::run_allowed({}, runs));
	WB_CHECK(wb::page::expand_home("~", "/h") == "/h" && wb::page::expand_home("~x", "/h") == "~x");
}

WB_TEST(approvals) {
	/* The file: lines kept, one replaced or added. */
	std::string text = "# mine\nmako aaa\n\nwaybar bbb\nmako-extra ccc\n";
	auto a = wb::page::parse_approvals(text);
	WB_CHECK(a.size() == 3 && a["mako"] == "aaa" && a["waybar"] == "bbb");
	std::string t2 = wb::page::approvals_with(text, "mako", "zzz");
	WB_CHECK(t2 == "# mine\nmako zzz\n\nwaybar bbb\nmako-extra ccc\n");
	std::string t3 = wb::page::approvals_with(t2, "fuzzel", "fff");
	WB_CHECK(wb::page::parse_approvals(t3)["fuzzel"] == "fff" &&
			wb::page::parse_approvals(t3)["mako"] == "zzz");
	/* A new file starts with a note saying what it is. */
	std::string fresh = wb::page::approvals_with("", "demo", "h1");
	WB_CHECK(fresh.rfind("# ", 0) == 0 && wb::page::parse_approvals(fresh)["demo"] == "h1");
	/* Odd lines: ignored. */
	WB_CHECK(wb::page::parse_approvals("lonely\n  # x y\n\tid  hash  extra\n")["id"] == "hash");
}

WB_TEST(page_sections) {
	auto v = wb::json::parse(R"({"controls": [
		{"kind": "number", "key": "timeout", "value": "5"},
		{"kind": "sections", "key": "rules", "label": "Rules",
		 "value": "[{\"name\":\"urgency=high\",\"values\":{\"timeout\":\"0\"}}]",
		 "choices": ["urgency=high", ["app-name=", "A program"]],
		 "children": [
			{"kind": "number", "key": "timeout", "label": "Disappear after"},
			{"kind": "color", "key": "background-color"}]}]})");
	std::string err;
	auto p = wb::page::page_from_json(*v, &err);
	WB_CHECK(p && err.empty());
	if (!p)
		return;
	/* Its fields' keys are its own: "timeout" twice is fine. */
	auto vals = wb::page::page_values(*p);
	WB_CHECK(vals.size() == 2 && vals[0].first == "timeout" && vals[1].first == "rules");
	WB_CHECK(wb::page::find_control(*p, "background-color") == nullptr);
	WB_CHECK(wb::page::find_control(*p, "rules")->children.size() == 2);
	WB_CHECK(wb::page::page_from_json(wb::page::page_to_json(*p)) == p);
	/* Only settings as fields. */
	auto bad = wb::json::parse(R"({"controls": [{"kind": "sections", "key": "s", "children": [
		{"kind": "button", "key": "b", "label": "B"}]}]})");
	WB_CHECK(!wb::page::page_from_json(*bad, &err) && err.find("only have settings") !=
			std::string::npos);
	/* The value. */
	auto secs = wb::page::sections_from_text(p->controls[1].value);
	WB_CHECK(secs.size() == 1 && secs[0].name == "urgency=high" &&
			secs[0].values.at("timeout") == "0");
	secs.push_back({"app-name=firefox", {{"timeout", "2000"}, {"background-color", ""}}});
	std::string text = wb::page::sections_to_text(secs);
	auto back = wb::page::sections_from_text(text);
	WB_CHECK(back.size() == 2 && back[1].values.size() == 1);  /* empty left out */
	WB_CHECK(wb::page::sections_from_text("").empty() &&
			wb::page::sections_from_text("{}").empty());
}

WB_TEST(page_font_monospace) {
	auto v = wb::json::parse(R"({"controls": [{"kind": "font", "key": "f", "format": "fontconfig",
		"monospace": true, "value": "monospace:size=10"}]})");
	auto p = wb::page::page_from_json(*v);
	WB_CHECK(p && p->controls[0].monospace && p->controls[0].format == "fontconfig");
	WB_CHECK(wb::page::page_from_json(wb::page::page_to_json(*p)) == p);
}

