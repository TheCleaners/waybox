#include "wb_test.hpp"

#include "waybox/xcursor_themes.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Tree {
	fs::path root;
	Tree() {
		char tmpl[] = "/tmp/wb-xcursor-XXXXXX";
		root = mkdtemp(tmpl);
	}
	~Tree() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	std::string dir(const std::string &name) {
		fs::create_directories(root / name);
		return (root / name).string();
	}
	void cursors(const std::string &dir, const std::string &theme) {
		fs::create_directories(root / dir / theme / "cursors");
		std::ofstream(root / dir / theme / "cursors" / "left_ptr") << "Xcur";
	}
	void index(const std::string &dir, const std::string &theme, const std::string &body) {
		fs::create_directories(root / dir / theme);
		std::ofstream(root / dir / theme / "index.theme") << body;
	}
};

}  // namespace

WB_TEST(search_path) {
	auto p = wb::xcursor_search_path(nullptr, nullptr, "/home/u");
	WB_CHECK(!p.empty() && p[0] == "/home/u/.local/share/icons");
	WB_CHECK(p.size() > 2 && p[1] == "/home/u/.icons" && p[2] == "/usr/share/icons");
	/* A relative XDG_DATA_HOME is ignored, as libXcursor does. */
	WB_CHECK(wb::xcursor_search_path(nullptr, "rel", "/h")[0] == "/h/.local/share/icons");
	WB_CHECK(wb::xcursor_search_path(nullptr, "/x", "/h")[0] == "/x/icons");
	/* XCURSOR_PATH replaces it all; empty parts skipped. */
	auto e = wb::xcursor_search_path("~/a::/b", "/x", "/h");
	WB_CHECK(e.size() == 2 && e[0] == "/h/a" && e[1] == "/b");
}

WB_TEST(empty_default_hides_the_system_one) {
	Tree t;
	std::vector<std::string> path = {t.dir("home"), t.dir("sys")};
	t.cursors("sys", "Adwaita");
	t.index("sys", "default", "[Icon Theme]\nInherits=Adwaita\n");
	/* The system's default: Adwaita's cursors. */
	WB_CHECK(wb::xcursor_theme_check("", path).has_cursors);
	/* An empty Inherits= in home is read first and ends the search. */
	t.index("home", "default", "# written by a tool\n[Icon Theme]\nName=Default\nInherits=\n");
	auto c = wb::xcursor_theme_check("default", path);
	WB_CHECK(!c.has_cursors);
	WB_CHECK(c.index_file == (t.root / "home" / "default" / "index.theme").string());
	WB_CHECK(c.inherits.empty());
	WB_CHECK(wb::xcursor_fallback_theme(path) == "Adwaita");
	/* Pointed at a theme with cursors, it has them again. */
	t.index("home", "default", "[Icon Theme]\nInherits = breeze_cursors\n");
	t.cursors("sys", "breeze_cursors");
	c = wb::xcursor_theme_check("default", path);
	WB_CHECK(c.has_cursors && c.inherits.size() == 1 && c.inherits[0] == "breeze_cursors");
}

WB_TEST(inherits_chains_cycles_and_merges) {
	Tree t;
	std::vector<std::string> path = {t.dir("a"), t.dir("b")};
	/* Separators: commas, semicolons, blanks; the second parent has them. */
	t.index("a", "Mine", "Inherits=Missing; Other ,\n");
	t.cursors("b", "Other");
	WB_CHECK(wb::xcursor_theme_check("Mine", path).has_cursors);
	/* A cycle ends, without cursors. */
	t.index("a", "Loop1", "Inherits=Loop2\n");
	t.index("a", "Loop2", "Inherits=Loop1\n");
	WB_CHECK(!wb::xcursor_theme_check("Loop1", path).has_cursors);
	/* Cursors in a later directory count (they merge). */
	t.index("a", "Split", "Inherits=\n");
	t.cursors("b", "Split");
	WB_CHECK(wb::xcursor_theme_check("Split", path).has_cursors);
	/* An empty cursors/ directory is no cursors. */
	fs::create_directories(t.root / "a" / "Hollow" / "cursors");
	WB_CHECK(!wb::xcursor_theme_check("Hollow", path).has_cursors);
	/* Not installed at all. */
	auto none = wb::xcursor_theme_check("Nowhere", path);
	WB_CHECK(!none.has_cursors && none.index_file.empty());
	auto themes = wb::xcursor_themes_with_cursors(path);
	WB_CHECK((themes == std::vector<std::string>{"Other", "Split"}));
	/* No Adwaita: the first by name. */
	WB_CHECK(wb::xcursor_fallback_theme(path) == "Other");
	WB_CHECK(wb::xcursor_fallback_theme({t.dir("empty")}).empty());
}
