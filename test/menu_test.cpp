#include "wb_test.hpp"

#include "waybox/menu.hpp"

using wb::Menu;
using wb::MenuItem;
using wb::MenuLayout;
using wb::MenuMetrics;

namespace {

/* A deterministic fake text measurer: 7px per character. */
auto fake_width() {
	return [](std::string_view s) { return static_cast<int>(s.size()) * 7; };
}

Menu sample() {
	Menu m;
	m.id = "root";
	m.items.push_back(MenuItem{MenuItem::Kind::Entry, "Terminal", {}, ""});
	m.items.push_back(MenuItem{MenuItem::Kind::Separator, "", {}, ""});
	m.items.push_back(MenuItem{MenuItem::Kind::Submenu, "Apps", {}, "apps"});
	return m;
}

}  // namespace

WB_TEST(menufile_find) {
	wb::MenuFile f;
	f.menus.push_back(Menu{"root", "Root", {}});
	f.menus.push_back(Menu{"apps", "Apps", {}});
	WB_CHECK(f.find("root") != nullptr);
	WB_CHECK(f.find("apps")->label == "Apps");
	WB_CHECK(f.find("missing") == nullptr);
}

WB_TEST(layout_width_is_widest_label_plus_padding) {
	Menu m;
	m.items.push_back(MenuItem{MenuItem::Kind::Entry, "OK", {}, ""});         /* 2ch */
	m.items.push_back(MenuItem{MenuItem::Kind::Entry, "Terminal", {}, ""});   /* 8ch */
	MenuMetrics metrics;
	metrics.min_width = 0;
	metrics.border = 0;
	MenuLayout l = wb::layout_menu(m, metrics, fake_width());
	/* widest = "Terminal" = 8*7 + 2*pad_x */
	WB_CHECK(l.width == 8 * 7 + 2 * metrics.pad_x);
}

WB_TEST(layout_respects_min_width_and_border) {
	Menu m;
	m.items.push_back(MenuItem{MenuItem::Kind::Entry, "x", {}, ""});
	MenuMetrics metrics;
	metrics.min_width = 200;
	metrics.border = 2;
	MenuLayout l = wb::layout_menu(m, metrics, fake_width());
	WB_CHECK(l.width == 200 + 2 * 2);  /* min content + both borders */
}

WB_TEST(layout_submenu_reserves_arrow_width) {
	Menu only_entry;
	only_entry.items.push_back(MenuItem{MenuItem::Kind::Entry, "Apps", {}, ""});
	Menu submenu;
	submenu.items.push_back(MenuItem{MenuItem::Kind::Submenu, "Apps", {}, "apps"});
	MenuMetrics metrics;
	metrics.min_width = 0;
	metrics.border = 0;
	int w_entry = wb::layout_menu(only_entry, metrics, fake_width()).width;
	int w_submenu = wb::layout_menu(submenu, metrics, fake_width()).width;
	WB_CHECK(w_submenu == w_entry + metrics.submenu_arrow_width);
}

WB_TEST(layout_height_sums_rows) {
	Menu m = sample();  /* entry + separator + submenu */
	MenuMetrics metrics;
	metrics.border = 0;
	MenuLayout l = wb::layout_menu(m, metrics, fake_width());
	WB_CHECK(l.height == metrics.item_height + metrics.separator_height +
			metrics.item_height);
	WB_CHECK(l.item_rects.size() == 3);
}

WB_TEST(hit_test_selects_items_and_skips_separators) {
	Menu m = sample();
	MenuMetrics metrics;
	metrics.border = 0;
	MenuLayout l = wb::layout_menu(m, metrics, fake_width());

	/* first row: the Terminal entry */
	WB_CHECK(wb::menu_item_at(m, l, 5, 1) == 0);
	/* second row: the separator -> not selectable */
	int sep_y = metrics.item_height + 1;
	WB_CHECK(wb::menu_item_at(m, l, 5, sep_y) == -1);
	/* third row: the Apps submenu */
	int sub_y = metrics.item_height + metrics.separator_height + 1;
	WB_CHECK(wb::menu_item_at(m, l, 5, sub_y) == 2);
	/* outside the menu */
	WB_CHECK(wb::menu_item_at(m, l, 5, l.height + 10) == -1);
	WB_CHECK(wb::menu_item_at(m, l, -1, 1) == -1);
}

WB_TEST(place_root_menu_keeps_on_screen) {
	wb::Rect bounds{0, 0, 1000, 800};

	/* fits at the pointer: top-left at the anchor */
	wb::Rect r = wb::place_root_menu(100, 100, 150, 200, bounds);
	WB_CHECK(r.x == 100 && r.y == 100 && r.width == 150 && r.height == 200);

	/* near the right edge: flips to the left of the pointer */
	wb::Rect right = wb::place_root_menu(950, 100, 150, 200, bounds);
	WB_CHECK(right.x == 800);  /* 950 - 150 */

	/* near the bottom edge: shifted up to stay on screen */
	wb::Rect bottom = wb::place_root_menu(100, 750, 150, 200, bounds);
	WB_CHECK(bottom.y == 600);  /* 800 - 200 */

	/* respects a non-zero bounds origin (multi-output layout) */
	wb::Rect off{200, 100, 500, 400};
	wb::Rect clamped = wb::place_root_menu(180, 90, 150, 200, off);
	WB_CHECK(clamped.x == 200 && clamped.y == 100);
}

WB_TEST(place_submenu_opens_right_then_flips_left) {
	wb::Rect bounds{0, 0, 1000, 800};
	wb::Rect parent{100, 100, 200, 300};  /* parent menu rect */

	/* opens to the right, overlapping the parent border by 3 */
	wb::Rect right = wb::place_submenu(parent, 140, 150, 200, bounds, 3);
	WB_CHECK(right.x == 297);   /* 100 + 200 - 3 */
	WB_CHECK(right.y == 140);

	/* a wide parent near the right edge: submenu flips to the left */
	wb::Rect near_right{780, 100, 200, 300};
	wb::Rect left = wb::place_submenu(near_right, 140, 150, 200, bounds, 3);
	WB_CHECK(left.x == 633);  /* 780 - 150 + 3 */

	/* item low on screen: submenu shifted up */
	wb::Rect low = wb::place_submenu(parent, 700, 150, 200, bounds, 0);
	WB_CHECK(low.y == 600);  /* 800 - 200 */
}
