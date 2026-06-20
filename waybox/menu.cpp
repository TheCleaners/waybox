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

MenuSource menu_source_from_config(std::string_view source) {
	/* Empty or "builtin" => native menu. A bare program name or full command
	 * line => delegate to that external launcher. */
	if (source.empty() || source == "builtin")
		return MenuSource{MenuSource::Kind::Builtin, {}};
	return MenuSource{MenuSource::Kind::External, std::string(source)};
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

	int y = m.border + m.pad_y;
	for (const MenuItem &item : menu.items) {
		int h = item_row_height(item, m);
		layout.item_rects.push_back(Rect{m.border, y, content_width, h});
		y += h;
	}

	layout.width = content_width + 2 * m.border;
	layout.height = y + m.pad_y + m.border;
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

int menu_step_selection(const Menu &menu, int current, int dir, bool wrap) {
	const int n = static_cast<int>(menu.items.size());
	if (n == 0 || dir == 0)
		return -1;
	auto selectable = [&](int i) {
		return i >= 0 && i < n &&
				menu.items[i].kind != MenuItem::Kind::Separator;
	};

	/* A -1 start sits just outside the appropriate end so the first step lands
	 * on the first (dir>0) or last (dir<0) item. */
	int i = current;
	if (current < 0)
		i = dir > 0 ? -1 : n;

	for (int step = 0; step < n; ++step) {
		i += dir;
		if (i < 0 || i >= n) {
			if (!wrap)
				break;
			i = (i + n) % n;
		}
		if (selectable(i))
			return i;
	}
	/* No move found: keep a valid current, else fall back to any selectable. */
	if (selectable(current))
		return current;
	for (int k = 0; k < n; ++k)
		if (selectable(k))
			return k;
	return -1;
}

static int clamp(int v, int lo, int hi) {
	if (hi < lo)  /* menu taller/wider than bounds: pin to the top/left edge */
		return lo;
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

Rect place_root_menu(int x, int y, int w, int h, const Rect &bounds) {
	if (x + w > bounds.x + bounds.width)
		x -= w;  /* flip to the left of the pointer */
	if (y + h > bounds.y + bounds.height)
		y = bounds.y + bounds.height - h;
	x = clamp(x, bounds.x, bounds.x + bounds.width - w);
	y = clamp(y, bounds.y, bounds.y + bounds.height - h);
	return Rect{x, y, w, h};
}

Rect place_submenu(const Rect &parent, int item_y, int w, int h,
		const Rect &bounds, int overlap) {
	int x = parent.x + parent.width - overlap;  /* open to the right */
	if (x + w > bounds.x + bounds.width)
		x = parent.x - w + overlap;  /* flip to the left */
	int y = item_y;
	if (y + h > bounds.y + bounds.height)
		y = bounds.y + bounds.height - h;
	x = clamp(x, bounds.x, bounds.x + bounds.width - w);
	y = clamp(y, bounds.y, bounds.y + bounds.height - h);
	return Rect{x, y, w, h};
}

}  // namespace wb
