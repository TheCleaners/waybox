#include "wb_test.hpp"

#include "waybox/ini_doc.hpp"

#include <string>

using wb::ini::Document;

namespace {

/* mako's own example, abridged: global keys, comments, colors with '#',
 * criteria sections, one of them twice. */
const char *kMako =
	"# Global\n"
	"font=monospace 10\n"
	"background-color = #285577\n"
	"default-timeout=5000\n"
	"\n"
	"; comment, another style\n"
	"[urgency=high]\n"
	"border-color=#bf616a\n"
	"\n"
	"[app-name=firefox]\n"
	"default-timeout=2000\n"
	"\n"
	"[urgency=high]\n"
	"ignore-timeout=1\n";

}  // namespace

WB_TEST(ini_round_trips_untouched) {
	for (const char *t : {kMako, "", "\n\n", "no newline=at end", "  # only a comment\n",
			"odd line without equals\n[s]\n=no key\n"})
		WB_CHECK(Document::parse(t).text() == t);
}

WB_TEST(ini_reads) {
	auto d = Document::parse(kMako);
	WB_CHECK(d.get("", "font") == "monospace 10");
	WB_CHECK(d.get("", "background-color") == "#285577");  /* '#' is the value's */
	WB_CHECK(d.get("app-name=firefox", "default-timeout") == "2000");
	WB_CHECK(d.get("urgency=high", "ignore-timeout") == "1");
	WB_CHECK(d.get("urgency=high", "border-color") == "#bf616a");  /* both [urgency=high] */
	WB_CHECK(!d.get("", "nope") && !d.get("missing", "font"));
	auto secs = d.sections();
	WB_CHECK((secs == std::vector<std::string>{"", "urgency=high", "app-name=firefox"}));
	auto g = d.entries("");
	WB_CHECK(g.size() == 3 && g[0].first == "font" && g[2].second == "5000");
}

WB_TEST(ini_changes_only_what_it_must) {
	auto d = Document::parse(kMako);
	d.set("", "default-timeout", "8000");
	d.set("", "background-color", "#000000");
	std::string t = d.text();
	/* The line kept, its spacing too ("key = value"). */
	WB_CHECK(t.find("background-color = #000000\n") != std::string::npos);
	WB_CHECK(t.find("default-timeout=8000\n# ") == std::string::npos);
	WB_CHECK(t.find("default-timeout=8000\n\n; comment") != std::string::npos);
	/* Everything else as it was. */
	WB_CHECK(t.find("# Global\nfont=monospace 10\n") == 0);
	WB_CHECK(t.find("[app-name=firefox]\ndefault-timeout=2000\n") != std::string::npos);
	/* A repeated section: the key changed where it last appears. */
	d.set("urgency=high", "ignore-timeout", "0");
	WB_CHECK(d.text().find("[urgency=high]\nignore-timeout=0\n") != std::string::npos);
}

WB_TEST(ini_adds_where_it_belongs) {
	auto d = Document::parse(kMako);
	d.set("", "width", "400");
	std::string t = d.text();
	/* After the last global key, before the comment and first section. */
	WB_CHECK(t.find("default-timeout=5000\nwidth=400\n\n; comment") != std::string::npos);
	d.set("app-name=firefox", "invisible", "1");
	WB_CHECK(d.text().find("default-timeout=2000\ninvisible=1\n") != std::string::npos);
	d.set("mode=do-not-disturb", "invisible", "1");
	WB_CHECK(d.text().size() > t.size());
	WB_CHECK(d.text().rfind("\n\n[mode=do-not-disturb]\ninvisible=1\n") ==
			d.text().size() - std::string("\n\n[mode=do-not-disturb]\ninvisible=1\n").size());
	/* A file with sections only: a global key goes above the first one's
	 * comments. */
	auto s = Document::parse("# foot\n[main]\nfont=x\n");
	s.set("", "top", "1");
	WB_CHECK(s.text() == "top=1\n# foot\n[main]\nfont=x\n");
	/* An empty file. */
	auto e = Document::parse("");
	e.set("", "a", "1");
	e.set("s", "b", "2");
	WB_CHECK(e.text() == "a=1\n\n[s]\nb=2\n");
	/* No final newline: kept, unless something's added at the end. */
	auto n = Document::parse("a=1");
	n.set("", "a", "2");
	WB_CHECK(n.text() == "a=2");
}

WB_TEST(ini_removes) {
	auto d = Document::parse(kMako);
	WB_CHECK(d.remove("", "font"));
	WB_CHECK(!d.get("", "font") && d.text().find("font=") == std::string::npos);
	WB_CHECK(!d.remove("", "font"));
	/* From every [urgency=high]. */
	auto m = Document::parse("[a]\nk=1\n[b]\nk=2\n[a]\nk=3\n");
	WB_CHECK(m.remove("a", "k") && m.text() == "[a]\n[b]\nk=2\n[a]\n");
}
