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

namespace {

/* A trimmed Breeze-style index: <context>/<size> layout, scalable dirs and
 * a HiDPI directory, inheriting hicolor via an intermediate theme. */
constexpr const char *kBreezeIndex = R"([Icon Theme]
Name=Breeze
Inherits=breeze-base,hicolor
Directories=categories/22,categories/24,categories/32,apps/48,apps/scalable,apps/48@2x

[categories/22]
Size=22
Type=Fixed

[categories/24]
Size=24
Type=Fixed

[categories/32]
Size=32
Type=Fixed

[apps/48]
Size=48
Type=Threshold

[apps/scalable]
Size=48
MinSize=16
MaxSize=256
Type=Scalable

[apps/48@2x]
Size=48
Scale=2
Type=Fixed
)";

}  // namespace

WB_TEST(theme_index_parses_dirs_and_inherits) {
	wb::IconThemeIndex idx = wb::parse_icon_theme_index(kBreezeIndex);
	WB_CHECK(idx.inherits.size() == 2);
	WB_CHECK(idx.inherits[0] == "breeze-base");
	WB_CHECK(idx.dirs.size() == 6);
	WB_CHECK(idx.dirs[1].path == "categories/24");
	WB_CHECK(idx.dirs[1].type == wb::IconThemeDir::Type::Fixed);
	WB_CHECK(idx.dirs[4].type == wb::IconThemeDir::Type::Scalable);
	WB_CHECK(idx.dirs[4].min_size == 16 && idx.dirs[4].max_size == 256);
	WB_CHECK(idx.dirs[5].scale == 2);
	/* Threshold defaults: min/max = size, threshold 2. */
	WB_CHECK(idx.dirs[3].min_size == 48 && idx.dirs[3].threshold == 2);
}

WB_TEST(theme_dirs_ordered_by_size_match_and_distance) {
	wb::IconThemeIndex idx = wb::parse_icon_theme_index(kBreezeIndex);
	std::vector<std::string> d = wb::icon_theme_dirs_for_size(idx, 24);
	/* Exact Fixed match and the Scalable range (16..256) both match 24;
	 * then by distance: 22 (2) before 32 (8) before 48 (24-ish). */
	WB_CHECK(d.size() == 6);
	auto pos = [&](const char *p) {
		return std::find(d.begin(), d.end(), p) - d.begin();
	};
	WB_CHECK(pos("categories/24") < pos("categories/22"));
	WB_CHECK(pos("apps/scalable") < pos("categories/22"));
	WB_CHECK(pos("categories/22") < pos("categories/32"));
	WB_CHECK(pos("categories/32") < pos("apps/48"));
	WB_CHECK(d.back() == "apps/48@2x");  /* HiDPI dirs last */
}

WB_TEST(search_paths_follow_theme_index_and_inherits) {
	wb::IconThemeIndex breeze = wb::parse_icon_theme_index(kBreezeIndex);
	wb::IconThemeIndex base = wb::parse_icon_theme_index(
			"[Icon Theme]\nInherits=hicolor\nDirectories=places/24\n"
			"[places/24]\nSize=24\nType=Fixed\n");
	auto lookup = [&](std::string_view t) -> const wb::IconThemeIndex * {
		if (t == "breeze") return &breeze;
		if (t == "breeze-base") return &base;
		return nullptr;  /* hicolor: generic layout */
	};
	std::vector<std::string> bases = {"/usr/share/icons", "/usr/share/pixmaps"};
	std::vector<std::string> p = wb::icon_search_paths(
			"applications-office", "breeze", 24, bases, lookup);
	auto pos = [&](const std::string &s) {
		return std::find(p.begin(), p.end(), s) - p.begin();
	};
	const std::string breeze24 = "/usr/share/icons/breeze/categories/24/applications-office.svg";
	const std::string base24 = "/usr/share/icons/breeze-base/places/24/applications-office.png";
	const std::string hicolor = "/usr/share/icons/hicolor/24x24/apps/applications-office.png";
	const std::string pixmap = "/usr/share/pixmaps/applications-office.png";
	WB_CHECK(pos(breeze24) < static_cast<long>(p.size()));
	WB_CHECK(pos(breeze24) < pos(base24));   /* theme before its parent */
	WB_CHECK(pos(base24) < pos(hicolor));    /* parents before hicolor */
	WB_CHECK(pos(hicolor) < pos(pixmap));    /* pixmaps last */
	/* The old guessed layout is not used for indexed themes. */
	WB_CHECK(pos("/usr/share/icons/breeze/24x24/categories/applications-office.svg")
			== static_cast<long>(p.size()));
}

WB_TEST(desktop_entry_icon_parsing) {
	WB_CHECK(wb::desktop_entry_icon("[Desktop Entry]\nName=Foot\nIcon=foot\n") == "foot");
	WB_CHECK(wb::desktop_entry_icon("[Desktop Entry]\nIcon = /opt/x/x.png \r\n") ==
			"/opt/x/x.png");
	/* Localised keys and other groups don't count. */
	WB_CHECK(wb::desktop_entry_icon("[Desktop Entry]\nIcon[de]=a\nIcon=b\n") == "b");
	WB_CHECK(wb::desktop_entry_icon("[Desktop Action New]\nIcon=new\n"
			"[Desktop Entry]\nName=x\n").empty());
	WB_CHECK(wb::desktop_entry_icon("").empty());
}
