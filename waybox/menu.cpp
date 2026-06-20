/*
 * Pure menu model + layout. Standard-library only (no libxml/cairo/wlroots), so
 * it links into the standalone unit test.
 */
#include "waybox/menu.hpp"

#include <algorithm>

namespace wb {

const Menu *MenuFile::find(std::string_view id) const {
	for (const Menu &menu : menus) {
		if (menu.id == id)
			return &menu;
	}
	return nullptr;
}

static int item_row_height(const MenuItem &item, const MenuMetrics &m) {
	return item.kind == MenuItem::Kind::Separator ? m.separator_height
			: m.item_height;
}

MenuLayout layout_menu(const Menu &menu, const MenuMetrics &m,
		const std::function<int(std::string_view)> &text_width) {
	MenuLayout layout;

	/* Content width = widest label (+ arrow for submenus), clamped to min. */
	int content_width = m.min_width;
	for (const MenuItem &item : menu.items) {
		int w = text_width(item.label) + 2 * m.pad_x;
		if (item.kind == MenuItem::Kind::Submenu)
			w += m.submenu_arrow_width;
		content_width = std::max(content_width, w);
	}

	int y = m.border;
	for (const MenuItem &item : menu.items) {
		int h = item_row_height(item, m);
		layout.item_rects.push_back(Rect{m.border, y, content_width, h});
		y += h;
	}

	layout.width = content_width + 2 * m.border;
	layout.height = y + m.border;
	return layout;
}

int menu_item_at(const Menu &menu, const MenuLayout &layout, int x, int y) {
	const size_t n = std::min(menu.items.size(), layout.item_rects.size());
	for (size_t i = 0; i < n; ++i) {
		if (menu.items[i].kind == MenuItem::Kind::Separator)
			continue;
		const Rect &r = layout.item_rects[i];
		if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
			return static_cast<int>(i);
	}
	return -1;
}

}  // namespace wb
