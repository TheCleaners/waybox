#ifndef WB_MENU_HPP
#define WB_MENU_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/action.hpp"
#include "waybox/geometry.hpp"

namespace wb {

/*
 * Menu model + layout (Openbox menu.xml).
 *
 * The data model and layout geometry are pure (no libxml/cairo/wlroots) and
 * unit-tested; menu.xml parsing (libxml) lives in menu_parse.cpp and the
 * rendering/interaction in the menu widget. Layout takes a text-measuring
 * callback so it stays pure and testable (the real one uses Pango).
 */

struct MenuItem {
	enum class Kind {
		Entry,      /* a label that runs actions */
		Separator,  /* a divider (optionally labelled) */
		Submenu,    /* opens another menu (by id) */
	};

	Kind kind = Kind::Entry;
	std::string label;
	std::vector<Action> actions;  /* for Entry */
	std::string submenu_id;       /* for Submenu */
};

struct Menu {
	std::string id;
	std::string label;
	std::vector<MenuItem> items;
};

/* A parsed menu file: a set of menus addressable by id. */
struct MenuFile {
	std::vector<Menu> menus;
	const Menu *find(std::string_view id) const;
};

/* Theme-derived metrics the layout needs (logical pixels). */
struct MenuMetrics {
	int item_height = 20;
	int separator_height = 7;
	int pad_x = 8;                 /* horizontal text padding */
	int border = 1;
	int min_width = 80;
	int submenu_arrow_width = 16;  /* reserved space for the "submenu" arrow */
};

struct MenuLayout {
	int width = 0;
	int height = 0;
	std::vector<Rect> item_rects;  /* one per menu item, menu-local coords */
};

/* Compute the geometry of `menu`. `text_width` returns the logical pixel width
 * of a label as it will be drawn. */
MenuLayout layout_menu(const Menu &menu, const MenuMetrics &metrics,
		const std::function<int(std::string_view)> &text_width);

/* Index of the item whose rect contains (x, y), or -1. Separators are not
 * selectable, so they return -1 even when hit. */
int menu_item_at(const Menu &menu, const MenuLayout &layout, int x, int y);

/* Parse Openbox menu.xml contents into a MenuFile (defined in menu_parse.cpp,
 * which links libxml; declared here so the pure model stays libxml-free). */
MenuFile parse_menu_xml(std::string_view contents);

}  // namespace wb

#endif /* WB_MENU_HPP */
