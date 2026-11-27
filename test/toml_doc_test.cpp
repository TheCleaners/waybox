#include "wb_test.hpp"

#include "waybox/toml_doc.hpp"

#include <string>

using wb::toml::Document;
using wb::toml::Path;
using wb::toml::Value;

namespace {

/* As Alacritty's: comments, tables, a dotted key, an inline table, an
 * array of tables, a float. */
const char *kAlacritty =
	"# Alacritty\n"
	"live_config_reload = true\n"
	"\n"
	"[window]\n"
	"opacity = 0.9   # a bit see-through\n"
	"padding = { x = 4, y = 4 }\n"
	"\n"
	"[font]\n"
	"size = 11.0\n"
	"normal.family = \"Iosevka\"\n"
	"\n"
	"[[keyboard.bindings]]\n"
	"key = \"N\"\n"
	"mods = \"Control|Shift\"\n"
	"action = \"CreateNewWindow\"\n"
	"\n"
	"[[keyboard.bindings]]\n"
	"key = \"Q\"\n"
	"action = \"Quit\"\n";

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

bool ends_with(const std::string &text, const std::string &end) {
	return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

bool has(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

}  // namespace

WB_TEST(toml_round_trips_untouched) {
	for (const char *t : {kAlacritty, "", "\n", "# only\n", "a = 1", "[t]\n",
			"s = '''\nraw \\n\n'''\nm = \"\"\"\nx \\\n  y\"\"\"\n",
			"d = 1979-05-27 07:32:00Z\nn = [1, [2, 3], { a = 'x' }]\n"}) {
		auto d = Document::parse(t);
		WB_CHECK(d.ok() && d.text() == t);
	}
}

WB_TEST(toml_reads_however_spelled) {
	auto d = Document::parse(kAlacritty);
	WB_CHECK(d.ok());
	WB_CHECK(d.get({"live_config_reload"})->boolean);
	WB_CHECK(d.get({"window", "opacity"})->number == 0.9);
	WB_CHECK(d.get({"window", "padding", "x"})->integer == 4);
	WB_CHECK(d.get({"font", "size"})->type == Value::Type::Number);
	WB_CHECK(d.get({"font", "normal", "family"})->string == "Iosevka");
	WB_CHECK(d.get({"keyboard", "bindings", size_t{1}, "key"})->string == "Q");
	WB_CHECK(d.get({"keyboard", "bindings"})->items.size() == 2);
	WB_CHECK(!d.get({"keyboard", "bindings", size_t{2}}) && !d.get({"nope"}));
	/* Strings of every kind, numbers of every base, a date as its text. */
	auto s = Document::parse(
			"a = \"\\u00e9\\t\"\nb = 'C:\\path'\nc = \"\"\"\nx \\\n   y\"\"\"\nd = '''\nl1\nl2'''\n"
			"h = 0xff\no = 0o17\nbin = 0b101\nu = 1_000\nf = -1.5e3\ni = -inf\n"
			"when = 1979-05-27T07:32:00-08:00\nday = 1979-05-27\nat = 07:32:00\n");
	WB_CHECK(s.ok());
	WB_CHECK(s.get({"a"})->string == "\xC3\xA9\t");
	WB_CHECK(s.get({"b"})->string == "C:\\path");
	WB_CHECK(s.get({"c"})->string == "x y");
	WB_CHECK(s.get({"d"})->string == "l1\nl2");
	WB_CHECK(s.get({"h"})->integer == 255 && s.get({"o"})->integer == 15 &&
			s.get({"bin"})->integer == 5 && s.get({"u"})->integer == 1000);
	WB_CHECK(s.get({"f"})->number == -1500 && s.get({"i"})->number < -1e308);
	WB_CHECK(s.get({"when"})->string == "1979-05-27T07:32:00-08:00");
	WB_CHECK(s.get({"day"})->string == "1979-05-27" && s.get({"at"})->string == "07:32:00");
}

WB_TEST(toml_errors_say_where) {
	auto d = Document::parse("a = 1\nb = \n");
	WB_CHECK(!d.ok() && has(d.error(), "line 2"));
	WB_CHECK(!Document::parse("a = 1 b = 2\n").ok());
	WB_CHECK(!Document::parse("[t\n").ok());
	WB_CHECK(!Document::parse("s = \"open\n").ok());
	WB_CHECK(!Document::parse("x = [1, 2\n").ok());
	WB_CHECK(!Document::parse(std::string(10000, '[')).ok());
	auto bad = Document::parse("a = ");
	WB_CHECK(!bad.set({"a"}, integer(1)) && bad.text() == "a = ");
}

WB_TEST(toml_changes_in_place) {
	auto d = Document::parse(kAlacritty);
	WB_CHECK(d.set({"font", "size"}, integer(12)));
	WB_CHECK(has(d.text(), "[font]\nsize = 12.0\n"));  /* a float stays one */
	Value o;
	o.type = Value::Type::Number;
	o.number = 0.85;
	WB_CHECK(d.set({"window", "opacity"}, o));
	WB_CHECK(has(d.text(), "opacity = 0.85   # a bit see-through\n"));
	WB_CHECK(d.set({"window", "padding", "y"}, integer(8)));
	WB_CHECK(has(d.text(), "padding = { x = 4, y = 8 }\n"));
	WB_CHECK(d.set({"font", "normal", "family"}, str("Fira Code")));
	WB_CHECK(has(d.text(), "normal.family = \"Fira Code\"\n"));
	WB_CHECK(d.set({"keyboard", "bindings", size_t{1}, "key"}, str("W")));
	WB_CHECK(has(d.text(), "key = \"W\"\naction = \"Quit\"\n"));
	/* The rest as it was. */
	WB_CHECK(d.text().find("# Alacritty\nlive_config_reload = true\n\n[window]\n") == 0);
	WB_CHECK(d.ok());
}

WB_TEST(toml_adds_where_it_belongs) {
	auto d = Document::parse(kAlacritty);
	/* After the table's last key. */
	WB_CHECK(d.set({"font", "offset", "x"}, integer(1)));
	WB_CHECK(has(d.text(), "action = \"Quit\"\n\n[font.offset]\nx = 1\n"));
	WB_CHECK(d.set({"window", "decorations"}, str("None")));
	WB_CHECK(has(d.text(), "padding = { x = 4, y = 4 }\ndecorations = \"None\"\n\n[font]"));
	/* Into the dotted table that's there, not a [header] for it. */
	WB_CHECK(d.set({"font", "normal", "style"}, str("Medium")));
	WB_CHECK(has(d.text(), "normal.family = \"Iosevka\"\nnormal.style = \"Medium\"\n"));
	/* Into an inline table. */
	WB_CHECK(d.set({"window", "padding", "z"}, integer(0)));
	WB_CHECK(has(d.text(), "padding = { x = 4, y = 4, z = 0 }\n"));
	/* A new table: its own header, at the end. */
	WB_CHECK(d.set({"cursor", "style", "shape"}, str("Beam")));
	WB_CHECK(ends_with(d.text(), "\n\n[cursor.style]\nshape = \"Beam\"\n"));
	/* At the top: after its keys, before the first table. */
	WB_CHECK(d.set({"ipc_socket"}, str("x")));
	WB_CHECK(has(d.text(), "live_config_reload = true\nipc_socket = \"x\"\n\n[window]"));
	/* Into an existing array-of-tables element; not a new element. */
	WB_CHECK(d.set({"keyboard", "bindings", size_t{1}, "mods"}, str("Control")));
	WB_CHECK(has(d.text(), "key = \"Q\"\naction = \"Quit\"\nmods = \"Control\"\n"));
	WB_CHECK(!d.set({"keyboard", "bindings", size_t{2}, "key"}, str("Z")));
	/* Not through a value. */
	WB_CHECK(!d.set({"font", "size", "x"}, integer(1)));
	WB_CHECK(d.ok() && Document::parse(d.text()).ok());

	/* A file with only tables: a top key above the first one's comment. */
	auto t = Document::parse("# the window\n[window]\nopacity = 1.0\n");
	WB_CHECK(t.set({"a"}, integer(1)) && t.text() == "a = 1\n# the window\n[window]\nopacity = 1.0\n");
	/* Empty, and a key under an empty table. */
	auto e = Document::parse("");
	WB_CHECK(e.set({"a"}, integer(1)) && e.text() == "a = 1\n");
	WB_CHECK(e.set({"t", "b"}, integer(2)) && e.text() == "a = 1\n\n[t]\nb = 2\n");
	auto h = Document::parse("[t]\n\n[u]\n");
	WB_CHECK(h.set({"t", "k"}, integer(1)) && h.text() == "[t]\nk = 1\n\n[u]\n");
	/* Indented keys: the same indentation. */
	auto in = Document::parse("[t]\n  a = 1\n");
	WB_CHECK(in.set({"t", "b"}, integer(2)) && in.text() == "[t]\n  a = 1\n  b = 2\n");
	/* An empty inline table. */
	auto it = Document::parse("p = {}\n");
	WB_CHECK(it.set({"p", "x"}, integer(1)) && it.text() == "p = { x = 1 }\n");
	/* Null: no such thing in TOML. */
	WB_CHECK(!e.set({"n"}, Value{}));
}

WB_TEST(toml_removes) {
	auto d = Document::parse(kAlacritty);
	WB_CHECK(d.remove({"window", "opacity"}));
	WB_CHECK(has(d.text(), "[window]\npadding = { x = 4, y = 4 }\n") && d.ok());
	WB_CHECK(d.remove({"window", "padding", "x"}));
	WB_CHECK(has(d.text(), "padding = { y = 4 }\n"));
	WB_CHECK(d.remove({"window", "padding", "y"}));
	WB_CHECK(has(d.text(), "padding = {}\n"));
	WB_CHECK(d.remove({"keyboard", "bindings", size_t{0}, "mods"}));
	WB_CHECK(has(d.text(), "key = \"N\"\naction = \"CreateNewWindow\"\n"));
	WB_CHECK(!d.remove({"nope"}) && !d.remove({"font"}));
	WB_CHECK(Document::parse(d.text()).ok());
}

WB_TEST(toml_serializes) {
	Value arr;
	arr.type = Value::Type::Array;
	arr.items = {integer(1), str("a\"b")};
	WB_CHECK(wb::toml::serialize(arr) == "[1, \"a\\\"b\"]");
	Value obj;
	obj.type = Value::Type::Object;
	obj.members.emplace_back("x", integer(1));
	obj.members.emplace_back("a b", integer(2));
	WB_CHECK(wb::toml::serialize(obj) == "{ x = 1, \"a b\" = 2 }");
	WB_CHECK(wb::toml::key_text("ok_-9") == "ok_-9" && wb::toml::key_text("") == "\"\"");
}

WB_TEST(toml_survives_broken_files) {
	std::string a = kAlacritty;
	auto exercise = [](const std::string &t) {
		auto d = Document::parse(t);
		if (!d.ok())
			return;
		Value v;
		v.type = Value::Type::Integer;
		v.integer = 7;
		d.set({"font", "size"}, v);
		d.set({"window", "padding", "z"}, v);
		d.set({"cursor", "blink"}, v);
		d.remove({"window", "opacity"});
		WB_CHECK(Document::parse(d.text()).ok());
	};
	for (size_t n = 0; n <= a.size(); ++n)
		exercise(a.substr(0, n));
	for (size_t i = 0; i < a.size(); ++i)
		exercise(a.substr(0, i) + a.substr(i + 1));
}

/* A new nested key inside an element of an array of tables: a dotted key
 * in that element (a [header] couldn't name it, and building one threw). */
WB_TEST(toml_nested_key_in_array_element) {
	auto d = Document::parse("[[bind]]\nkey = \"a\"\n\n[[bind]]\nkey = \"b\"\n");
	WB_CHECK(d.ok());
	WB_CHECK(d.set(Path{std::string("bind"), size_t{0}, std::string("mods"),
			std::string("shift")}, integer(1)));
	WB_CHECK(d.ok());
	WB_CHECK(d.text().find("[[bind]]\nkey = \"a\"\nmods.shift = 1\n") != std::string::npos);
	WB_CHECK(d.text().find("[bind.mods]") == std::string::npos);
}
