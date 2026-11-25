#include "wb_test.hpp"

#include "waybox/gtk_appearance.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
struct TempDir {
	fs::path path;
	TempDir() {
		char tmpl[] = "/tmp/wb-gtkapp-XXXXXX";
		path = mkdtemp(tmpl);
	}
	~TempDir() { fs::remove_all(path); }
	void theme(const std::string &dir, const std::string &id, const std::string &index) {
		fs::create_directories(path / dir / id);
		std::ofstream(path / dir / id / "index.theme") << index;
	}
};
}  // namespace

WB_TEST(missing_fallbacks_follow_the_chain) {
	TempDir t;
	t.theme("user", "Cosmic", "[Icon Theme]\nName=Cosmic\nInherits=Pop,hicolor\nDirectories=a\n");
	t.theme("sys", "Adwaita", "[Icon Theme]\nName=Adwaita\nInherits=hicolor\nDirectories=a\n");
	t.theme("sys", "hicolor", "[Icon Theme]\nName=Hicolor\nDirectories=a\n");
	auto themes = wb::find_icon_themes({(t.path / "user").string(), (t.path / "sys").string()});
	WB_CHECK(wb::missing_fallbacks("Cosmic", themes) == std::vector<std::string>{"Pop"});
	WB_CHECK(wb::missing_fallbacks("Adwaita", themes).empty());
	WB_CHECK(wb::complete_icon_theme(themes) == "Adwaita");
	/* Filled with a stand-in: no gaps left, and it's known as one. */
	t.theme("user", "Pop", wb::stand_in_theme("Pop", "Adwaita"));
	themes = wb::find_icon_themes({(t.path / "user").string(), (t.path / "sys").string()});
	WB_CHECK(wb::missing_fallbacks("Cosmic", themes).empty());
	WB_CHECK(themes.at("Pop").stand_in && themes.at("Pop").hidden && !themes.at("Pop").has_icons);
	WB_CHECK(!themes.at("Pop").hides_real);
	/* The real Pop installed later: the stand-in now hides it. */
	t.theme("sys", "Pop", "[Icon Theme]\nName=Pop\nInherits=hicolor\nDirectories=a\n");
	themes = wb::find_icon_themes({(t.path / "user").string(), (t.path / "sys").string()});
	WB_CHECK(themes.at("Pop").stand_in && themes.at("Pop").hides_real);
}

WB_TEST(missing_fallbacks_survive_cycles_and_name_the_theme_itself) {
	TempDir t;
	t.theme("d", "A", "[Icon Theme]\nInherits=B\nDirectories=x\n");
	t.theme("d", "B", "[Icon Theme]\nInherits=A,Gone\nDirectories=x\n");
	t.theme("d", "hicolor", "[Icon Theme]\nDirectories=x\n");
	auto themes = wb::find_icon_themes({(t.path / "d").string()});
	WB_CHECK(wb::missing_fallbacks("A", themes) == std::vector<std::string>{"Gone"});
	WB_CHECK(wb::missing_fallbacks("Nope", themes).front() == "Nope");
}

WB_TEST(the_first_directory_wins_and_cursor_themes_count) {
	TempDir t;
	t.theme("first", "X", "[Icon Theme]\nName=First\n");
	t.theme("second", "X", "[Icon Theme]\nName=Second\n");
	fs::create_directories(t.path / "second" / "Cur" / "cursors");
	auto themes = wb::find_icon_themes({(t.path / "first").string(), (t.path / "second").string()});
	WB_CHECK(themes.at("X").name == "First");
	WB_CHECK(themes.count("Cur") == 1 && themes.at("Cur").has_cursors);
}
