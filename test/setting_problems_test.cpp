#include "wb_test.hpp"

#include "waybox/setting_problems.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Icons {
	fs::path root;
	Icons() {
		char tmpl[] = "/tmp/wb-problems-XXXXXX";
		root = mkdtemp(tmpl);
	}
	~Icons() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	std::string dir(const std::string &d) {
		fs::create_directories(root / d);
		return (root / d).string();
	}
	void cursors(const std::string &d, const std::string &theme) {
		fs::create_directories(root / d / theme / "cursors");
		std::ofstream(root / d / theme / "cursors" / "left_ptr") << "Xcur";
	}
	std::string index(const std::string &d, const std::string &theme, const std::string &body) {
		fs::create_directories(root / d / theme);
		fs::path p = root / d / theme / "index.theme";
		std::ofstream(p) << body;
		return p.string();
	}
};

bool has(const std::string &s, const std::string &part) {
	return s.find(part) != std::string::npos;
}

}  // namespace

WB_TEST(tilde_shortens_home) {
	WB_CHECK(wb::tilde("/home/u/.local/share/x", "/home/u") == "~/.local/share/x");
	WB_CHECK(wb::tilde("/home/u/.local/share/x", "/home/u/") == "~/.local/share/x");
	WB_CHECK(wb::tilde("/home/u", "/home/u") == "~");
	/* Not a prefix by path: /home/user isn't under /home/u. */
	WB_CHECK(wb::tilde("/home/user/x", "/home/u") == "/home/user/x");
	WB_CHECK(wb::tilde("/usr/share/x", "/home/u") == "/usr/share/x");
	WB_CHECK(wb::tilde("/x", "") == "/x");
	WB_CHECK(wb::tilde("/x", "/") == "/x");
}

WB_TEST(written_by_reads_the_comment) {
	Icons t;
	WB_CHECK(wb::written_by(t.index("a", "x", "# This file is written by nwg-look. Do not edit.\n"
			"[Icon Theme]\n")) == "nwg-look");
	WB_CHECK(wb::written_by(t.index("a", "y", "[Icon Theme]\nName=Default\n")).empty());
	/* A comment past the first lines, or not a comment: not taken. */
	WB_CHECK(wb::written_by(t.index("a", "z", "Name=written by nobody.\n")).empty());
	WB_CHECK(wb::written_by((t.root / "missing").string()).empty());
	/* Too long to be a program's name. */
	WB_CHECK(wb::written_by(t.index("a", "w", "# written by " + std::string(80, 'x') + "\n"))
			.empty());
}

WB_TEST(pointer_default_without_cursors) {
	Icons t;
	std::vector<std::string> path = {t.dir("home"), t.dir("sys")};
	t.cursors("sys", "Adwaita");
	t.cursors("sys", "breeze_cursors");
	t.index("sys", "default", "[Icon Theme]\nInherits=Adwaita\n");
	wb::PointerState s{"breeze_cursors", std::nullopt, "", false, path, ""};
	/* The system default works; GTK's differs from it: said so. */
	auto mismatch = wb::pointer_problem(s);
	WB_CHECK(mismatch && has(mismatch->text, "\xE2\x80\x9C" "default\xE2\x80\x9D") &&
			has(mismatch->text, "breeze_cursors"));
	WB_CHECK(mismatch && mismatch->fix_label == "Use breeze_cursors for Waybox Too");
	/* A settings tool's empty "default" hides it: named, with its file. */
	std::string file = t.index("home", "default",
			"# This file is written by nwg-look. Do not edit.\n[Icon Theme]\nInherits=\n");
	auto broken = wb::pointer_problem(s);
	WB_CHECK(broken && has(broken->text, "has no cursors") && has(broken->text, file) &&
			has(broken->text, "written by nwg-look"));
	s.home = (t.root / "home").string();
	broken = wb::pointer_problem(s);
	WB_CHECK(broken && has(broken->text, "~/default/index.theme") && !has(broken->text, file) &&
			has(broken->text, "written by nwg-look") &&
			has(broken->text, "uses \xE2\x80\x9C" "Adwaita\xE2\x80\x9D"));
	WB_CHECK(broken && !broken->fixed_on_save && !broken->fix_label.empty());
	/* The fix chosen, not saved yet: no button, "fixed on save". */
	s.to_be_written = true;
	auto pending = wb::pointer_problem(s);
	WB_CHECK(pending && pending->fixed_on_save && pending->fix_label.empty());
	/* Saved: Waybox uses GTK's theme; nothing to say. */
	s.rc_theme = "breeze_cursors";
	s.to_be_written = false;
	WB_CHECK(!wb::pointer_problem(s));
	/* Started with XCURSOR_THEME (Waybox's own fallback, say): that's what
	 * it draws. */
	s.rc_theme.reset();
	s.env_theme = "Adwaita";
	auto env = wb::pointer_problem(s);
	WB_CHECK(env && has(env->text, "Adwaita") && !has(env->text, "no cursors"));
	s.env_theme = "breeze_cursors";
	WB_CHECK(!wb::pointer_problem(s));
}

WB_TEST(pointer_nothing_to_offer) {
	Icons t;
	std::vector<std::string> path = {t.dir("only")};
	/* Nothing has cursors: the stand-in, and no fix to offer. */
	wb::PointerState s{"", std::string("Gone"), "", false, path, ""};
	auto p = wb::pointer_problem(s);
	WB_CHECK(p && has(p->text, "stand-in") && p->fix_label.empty() && !p->fixed_on_save);
	/* GTK's theme without cursors either: not offered as the fix. */
	s.shown = "AlsoGone";
	p = wb::pointer_problem(s);
	WB_CHECK(p && p->fix_label.empty());
	/* rc.xml's theme fine, GTK's broken: not a mismatch worth a note
	 * (the GTK page warns about its own). */
	t.cursors("only", "Good");
	s.rc_theme = "Good";
	WB_CHECK(!wb::pointer_problem(s));
	/* A translation is used. */
	s.rc_theme = "Gone";
	auto tr = [](const char *m) -> const char * {
		return std::string(m) == "Waybox draws a stand-in pointer, whose size can't change."
				? "TRANSLATED" : m;
	};
	t.cursors("only", "Good");  /* the fallback exists now */
	fs::remove_all(t.root / "only" / "Good");
	p = wb::pointer_problem(s, tr);
	WB_CHECK(p && has(p->text, "TRANSLATED"));
}
