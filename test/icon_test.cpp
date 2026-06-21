#include "wb_test.hpp"

#include "waybox/icon.hpp"

#include <algorithm>

static bool has(const std::vector<std::string> &v, const std::string &s) {
	return std::find(v.begin(), v.end(), s) != v.end();
}

static bool any_contains(const std::vector<std::string> &v, const std::string &sub) {
	for (const auto &s : v)
		if (s.find(sub) != std::string::npos)
			return true;
	return false;
}

WB_TEST(base_dirs_follow_xdg_order) {
	auto b = wb::icon_base_dirs("/home/u", "/home/u/.local/share",
			"/usr/local/share:/usr/share");
	/* XDG_DATA_HOME/icons, ~/.icons, each XDG_DATA_DIRS/icons, then pixmaps */
	WB_CHECK(b.front() == "/home/u/.local/share/icons");
	WB_CHECK(has(b, "/home/u/.icons"));
	WB_CHECK(has(b, "/usr/local/share/icons"));
	WB_CHECK(has(b, "/usr/share/icons"));
	WB_CHECK(b.back() == "/usr/share/pixmaps");
}

WB_TEST(base_dirs_default_data_dirs) {
	auto b = wb::icon_base_dirs("/home/u", "", "");
	/* falls back to ~/.local/share/icons and the default data dirs */
	WB_CHECK(b.front() == "/home/u/.local/share/icons");
	WB_CHECK(has(b, "/usr/share/icons"));
}

WB_TEST(absolute_name_returned_verbatim) {
	auto p = wb::icon_search_paths("/opt/app/icon.png", "Adwaita", 24,
			{"/usr/share/icons", "/usr/share/pixmaps"});
	WB_CHECK(p.size() == 1);
	WB_CHECK(p.front() == "/opt/app/icon.png");
}

WB_TEST(search_paths_cover_theme_size_context_and_fallbacks) {
	std::vector<std::string> bases = {"/usr/share/icons", "/usr/share/pixmaps"};
	auto p = wb::icon_search_paths("firefox", "Adwaita", 24, bases);

	/* exact size + apps context + png in the named theme, preferred early */
	WB_CHECK(has(p, "/usr/share/icons/Adwaita/24x24/apps/firefox.png"));
	/* svg variant present */
	WB_CHECK(has(p, "/usr/share/icons/Adwaita/scalable/apps/firefox.svg"));
	/* hicolor fallback theme is searched */
	WB_CHECK(any_contains(p, "/usr/share/icons/hicolor/"));
	WB_CHECK(has(p, "/usr/share/icons/hicolor/48x48/apps/firefox.png"));
	/* legacy pixmaps fallback, flat (no theme/size) */
	WB_CHECK(has(p, "/usr/share/pixmaps/firefox.png"));
	WB_CHECK(has(p, "/usr/share/pixmaps/firefox.svg"));

	/* the exact-size apps/png candidate comes before the pixmaps fallback */
	auto idx = [&](const std::string &s) {
		return std::find(p.begin(), p.end(), s) - p.begin();
	};
	WB_CHECK(idx("/usr/share/icons/Adwaita/24x24/apps/firefox.png") <
			idx("/usr/share/pixmaps/firefox.png"));
}

WB_TEST(empty_name_yields_nothing) {
	WB_CHECK(wb::icon_search_paths("", "Adwaita", 24, {"/usr/share/icons"}).empty());
}
