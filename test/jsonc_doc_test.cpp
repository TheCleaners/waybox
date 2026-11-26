#include "wb_test.hpp"

#include "waybox/jsonc_doc.hpp"

#include <string>

using wb::jsonc::Document;
using wb::jsonc::Path;
using wb::jsonc::Value;

namespace {

/* As Waybar's own config: comments of both kinds, a trailing comma, an
 * object per module. */
const char *kWaybar =
	"// -*- mode: jsonc -*-\n"
	"{\n"
	"    \"layer\": \"top\", // waybar on top\n"
	"    \"height\": 30,\n"
	"    /* where */\n"
	"    \"modules-left\": [\"sway/workspaces\", \"sway/mode\"],\n"
	"    \"clock\": {\n"
	"        \"format\": \"{:%H:%M}\",\n"
	"        \"tooltip\": true,\n"
	"    },\n"
	"}\n";

Value str(const std::string &s) {
	Value v;
	v.type = Value::Type::String;
	v.string = s;
	return v;
}

Value integer(int64_t i) {
	Value v;
	v.type = Value::Type::Integer;
	v.integer = i;
	return v;
}

bool has(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

}  // namespace

WB_TEST(jsonc_round_trips_untouched) {
	for (const char *t : {kWaybar, "", "  \n", "[1, 2, 3]", "{}", "// only a comment\n",
			"{\"a\": {\"b\": [true, false, null, -1.5e3, \"\\u00e9\\n\"]}}"}) {
		auto d = Document::parse(t);
		WB_CHECK(d.ok() && d.text() == t);
	}
}

WB_TEST(jsonc_reads) {
	auto d = Document::parse(kWaybar);
	WB_CHECK(d.ok());
	WB_CHECK(d.get({"layer"})->string == "top");
	WB_CHECK(d.get({"height"})->type == Value::Type::Integer && d.get({"height"})->integer == 30);
	WB_CHECK(d.get({"modules-left", size_t{1}})->string == "sway/mode");
	WB_CHECK(d.get({"clock", "format"})->string == "{:%H:%M}");
	WB_CHECK(d.get({"clock", "tooltip"})->boolean);
	auto clock = d.get({"clock"});
	WB_CHECK(clock->members.size() == 2 && clock->members[0].first == "format");
	WB_CHECK(!d.get({"nope"}) && !d.get({"height", "x"}) && !d.get({"modules-left", size_t{2}}));
	/* Escapes and surrogate pairs. */
	auto e = Document::parse("[\"\\u00e9\\ud83d\\ude00\\t\"]");
	WB_CHECK(e.get({size_t{0}})->string == "\xC3\xA9\xF0\x9F\x98\x80\t");
	/* Numbers: whole ones stay whole. */
	auto n = Document::parse("[1, 1.5, -2, 1e3, 99999999999999999999]");
	WB_CHECK(n.get({size_t{0}})->type == Value::Type::Integer);
	WB_CHECK(n.get({size_t{1}})->number == 1.5);
	WB_CHECK(n.get({size_t{2}})->integer == -2);
	WB_CHECK(n.get({size_t{3}})->type == Value::Type::Number);
	WB_CHECK(n.get({size_t{4}})->type == Value::Type::Number);
}

WB_TEST(jsonc_errors_say_where) {
	auto d = Document::parse("{\n  \"a\": 1\n  \"b\": 2\n}");
	WB_CHECK(!d.ok() && has(d.error(), "line 3") && has(d.error(), "',' or '}'"));
	WB_CHECK(!Document::parse("{\"a\": 1} x").ok());
	WB_CHECK(!Document::parse("/* open").ok());
	WB_CHECK(!Document::parse("{'a': 1}").ok());
	/* Deep nesting: refused, not a crash. */
	WB_CHECK(!Document::parse(std::string(100000, '[')).ok());
	/* Nothing changes a document that didn't parse. */
	auto bad = Document::parse("{");
	WB_CHECK(!bad.set({"a"}, integer(1)) && bad.text() == "{");
}

WB_TEST(jsonc_changes_in_place) {
	auto d = Document::parse(kWaybar);
	WB_CHECK(d.set({"height"}, integer(32)));
	WB_CHECK(d.set({"layer"}, str("bottom")));
	WB_CHECK(d.set({"clock", "format"}, str("{:%a %H:%M}")));
	const std::string &t = d.text();
	WB_CHECK(has(t, "    \"height\": 32,\n"));
	WB_CHECK(has(t, "\"layer\": \"bottom\", // waybar on top\n"));
	WB_CHECK(has(t, "        \"format\": \"{:%a %H:%M}\",\n"));
	/* The rest as it was. */
	WB_CHECK(t.find("// -*- mode: jsonc -*-\n{\n") == 0);
	WB_CHECK(has(t, "    /* where */\n    \"modules-left\": [\"sway/workspaces\", \"sway/mode\"],\n"));
	/* An array replaced: on one line. */
	Value mods;
	mods.type = Value::Type::Array;
	mods.items = {str("sway/workspaces")};
	WB_CHECK(d.set({"modules-left"}, mods));
	WB_CHECK(has(d.text(), "\"modules-left\": [\"sway/workspaces\"],\n"));
}

WB_TEST(jsonc_adds_in_the_file_style) {
	auto d = Document::parse(kWaybar);
	/* At the end of the object, its indentation, its trailing comma. */
	WB_CHECK(d.set({"clock", "interval"}, integer(1)));
	WB_CHECK(has(d.text(), "        \"tooltip\": true,\n        \"interval\": 1,\n    },\n"));
	WB_CHECK(d.set({"spacing"}, integer(4)));
	WB_CHECK(has(d.text(), "    },\n    \"spacing\": 4,\n}\n"));
	/* Objects on the way made. */
	WB_CHECK(d.set({"battery", "states", "warning"}, integer(30)));
	WB_CHECK(has(d.text(), "    \"battery\": {\n        \"states\": {\n"
			"            \"warning\": 30\n        }\n    },\n}\n"));
	WB_CHECK(d.get({"battery", "states", "warning"})->integer == 30);
	/* Appended to an array. */
	WB_CHECK(d.set({"modules-left", size_t{2}}, str("custom/media")));
	WB_CHECK(has(d.text(), "[\"sway/workspaces\", \"sway/mode\", \"custom/media\"]"));
	WB_CHECK(!d.set({"modules-left", size_t{9}}, str("x")));
	WB_CHECK(!d.set({"height", "x"}, integer(1)));
	WB_CHECK(d.ok());

	/* No trailing comma: one added after the last, past its comment. */
	auto c = Document::parse("{\n  \"a\": 1 // one\n}\n");
	WB_CHECK(c.set({"b"}, integer(2)));
	WB_CHECK(c.text() == "{\n  \"a\": 1, // one\n  \"b\": 2\n}\n");
	/* On one line: kept on one line. */
	auto one = Document::parse("{\"a\": 1}");
	WB_CHECK(one.set({"b"}, integer(2)) && one.text() == "{\"a\": 1, \"b\": 2}");
	/* Empty objects, and an empty file. */
	auto e = Document::parse("{}");
	WB_CHECK(e.set({"a"}, integer(1)) && e.text() == "{\n    \"a\": 1\n}");
	auto blank = Document::parse("");
	WB_CHECK(blank.set({"a"}, integer(1)) && blank.text() == "{\n    \"a\": 1\n}\n");
	/* A file of only a comment, no final newline: the object after it. */
	auto comment = Document::parse("// c");
	WB_CHECK(comment.set({"a"}, integer(1)) && comment.text() == "// c\n{\n    \"a\": 1\n}\n");
	/* Tabs, if the file has them. */
	auto tabs = Document::parse("{\n\t\"a\": {}\n}\n");
	WB_CHECK(tabs.set({"a", "b"}, integer(1)));
	WB_CHECK(tabs.text() == "{\n\t\"a\": {\n\t\t\"b\": 1\n\t}\n}\n");
}

WB_TEST(jsonc_removes) {
	auto d = Document::parse(kWaybar);
	WB_CHECK(d.remove({"height"}));
	WB_CHECK(!has(d.text(), "height") && d.ok());
	WB_CHECK(has(d.text(), "\"layer\": \"top\", // waybar on top\n    /* where */\n"));
	/* The last one, with no comma after it: the one before it goes. */
	auto c = Document::parse("{\n  \"a\": 1,\n  \"b\": 2\n}\n");
	WB_CHECK(c.remove({"b"}) && c.text() == "{\n  \"a\": 1\n}\n" && c.ok());
	auto one = Document::parse("{\"a\": 1, \"b\": 2}");
	WB_CHECK(one.remove({"a"}) && one.text() == "{\"b\": 2}");
	auto arr = Document::parse("[1, 2, 3]");
	WB_CHECK(arr.remove({size_t{1}}) && arr.text() == "[1, 3]");
	WB_CHECK(arr.remove({size_t{1}}) && arr.text() == "[1]");
	WB_CHECK(arr.remove({size_t{0}}) && arr.text() == "[]");
	WB_CHECK(!arr.remove({size_t{0}}));
	/* A repeated key: all of them. */
	auto r = Document::parse("{\"a\": 1, \"b\": 2, \"a\": 3}");
	WB_CHECK(r.remove({"a"}) && r.text() == "{\"b\": 2}");
	WB_CHECK(!d.remove({"nope"}));
}

WB_TEST(jsonc_serializes) {
	Value obj;
	obj.type = Value::Type::Object;
	obj.members.emplace_back("format", str("{}"));
	Value arr;
	arr.type = Value::Type::Array;
	arr.items = {integer(1), obj};
	WB_CHECK(wb::jsonc::serialize(arr, "", "  ") ==
			"[\n  1,\n  {\n    \"format\": \"{}\"\n  }\n]");
	Value n;
	n.type = Value::Type::Number;
	n.number = 0.1;
	WB_CHECK(wb::jsonc::serialize(n) == "0.1");
	n.number = 2;
	WB_CHECK(wb::jsonc::serialize(n) == "2.0");
	WB_CHECK(wb::jsonc::serialize(str("a\"b\\\n")) == "\"a\\\"b\\\\\\n\"");
}

WB_TEST(jsonc_survives_broken_files) {
	/* Every prefix and every one-byte deletion of a real config: parsed or
	 * refused, never a crash; when parsed, edits keep it parseable. */
	std::string w = kWaybar;
	auto exercise = [](const std::string &t) {
		auto d = Document::parse(t);
		if (!d.ok())
			return;
		Value v;
		v.type = Value::Type::Integer;
		v.integer = 7;
		d.set({"height"}, v);
		d.set({"clock", "interval"}, v);
		d.remove({"layer"});
		d.remove({"modules-left", size_t{0}});
		WB_CHECK(Document::parse(d.text()).ok());
	};
	for (size_t n = 0; n <= w.size(); ++n)
		exercise(w.substr(0, n));
	for (size_t i = 0; i < w.size(); ++i)
		exercise(w.substr(0, i) + w.substr(i + 1));
}

WB_TEST(jsonc_lists_keep_their_lines) {
	const char *t =
		"{\n"
		"    \"modules-right\": [\n"
		"        \"tray\",\n"
		"        // \"cpu\",\n"
		"        \"pulseaudio\", // volume\n"
		"        \"network\",\n"
		"        \"clock\"\n"
		"    ],\n"
		"}\n";
	auto list = [](std::initializer_list<const char *> names) {
		Value v;
		v.type = Value::Type::Array;
		for (const char *n : names)
			v.items.push_back(str(n));
		return v;
	};
	/* Reordered, one removed, one added: lines and comments kept. */
	auto d = Document::parse(t);
	WB_CHECK(d.set({"modules-right"}, list({"clock", "pulseaudio", "tray", "battery"})));
	WB_CHECK(d.text() ==
		"{\n"
		"    \"modules-right\": [\n"
		"        \"clock\",\n"
		"        // \"cpu\",\n"
		"        \"pulseaudio\", // volume\n"
		"        \"tray\",\n"
		"        \"battery\"\n"
		"    ],\n"
		"}\n");
	/* The item a comment line is above, gone: the comment stays. */
	auto g = Document::parse(t);
	WB_CHECK(g.set({"modules-right"}, list({"tray", "network", "clock"})));
	WB_CHECK(g.text().find("        \"tray\",\n        // \"cpu\",\n        \"network\",") !=
			std::string::npos);
	WB_CHECK(g.text().find("pulseaudio") == std::string::npos && g.ok());
	/* A trailing comma after the last: kept so. */
	auto c = Document::parse("[\n  \"a\",\n  \"b\",\n]\n");
	WB_CHECK(c.set({}, list({"b", "a", "c"})));
	WB_CHECK(c.text() == "[\n  \"b\",\n  \"a\",\n  \"c\",\n]\n");
	/* On one line: replaced as it was. */
	auto o = Document::parse("{\"m\": [\"a\", \"b\"]}");
	WB_CHECK(o.set({"m"}, list({"b"})) && o.text() == "{\"m\": [\"b\"]}");
	/* Emptied: the comments stay, inside. */
	auto e = Document::parse(t);
	WB_CHECK(e.set({"modules-right"}, list({})));
	WB_CHECK(e.text().find("\"modules-right\": [\n        // \"cpu\",\n    ],") !=
			std::string::npos && e.ok());
}

