/*
 * Interactive root/context menu widget: the wlroots/Cairo glue around the pure
 * menu model. Renders one Cairo panel per open level into the scene graph and
 * routes grabbed input to selection/submenu navigation.
 */
#include "waybox/menu_widget.hpp"

#include <algorithm>
#include <cstdlib>

#include <cairo.h>

#include "waybox/icon.hpp"
#include "waybox/render.hpp"
#include "waybox/server.h"

namespace wb {

namespace {

/* Draw a small right-pointing triangle (the submenu arrow) centred in the
 * column reserved at the right of an item row. `w` is both the horizontal reach
 * and half the vertical extent, so the arrow stays visually balanced. */
void paint_arrow(cairo_t *cr, double x, double y, double h, double w,
		const Color &c) {
	double cy = y + h / 2.0;
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
	cairo_move_to(cr, x, cy - w);
	cairo_line_to(cr, x + w, cy);
	cairo_line_to(cr, x, cy + w);
	cairo_close_path(cr);
	cairo_fill(cr);
}

}  // namespace

MenuWidget::MenuWidget(struct wb_server *server, const MenuFile &menus,
		const MenuStyle &style, const MenuBehavior &behavior)
	: server_(server), menus_(menus), style_(style), behavior_(behavior),
	  metrics_(menu_metrics(style)) {
	/* Size rows from the font's real cell height (incl. ascenders/descenders),
	 * not the point size, so text is centred and descenders are never clipped.
	 * "Ag" exercises both extremes. Row height is computed dynamically from the
	 * measured font plus the style's compactness (item_pad_y); an explicit
	 * themed item_height, if larger, wins. */
	text_height_ = measure_text("Ag", style_.item_text.font).height;
	int comfortable = text_height_ + 2 * style_.item_pad_y;
	if (metrics_.item_height < comfortable)
		metrics_.item_height = comfortable;
	if (metrics_.pad_y < 2)
		metrics_.pad_y = 2;  /* always a little breathing room top/bottom */

	/* Horizontal text inset should scale with the font, not sit at a tiny fixed
	 * themerc value: a label looks cramped if the side padding is much smaller
	 * than the glyph height. Keep at least ~0.6x the line height. */
	int comfortable_x = (text_height_ * 3) / 5;
	if (metrics_.pad_x < comfortable_x)
		metrics_.pad_x = comfortable_x;

	/* Icons are square, sized to the row's text height; the column also gets a
	 * little padding to the right of the icon. */
	icon_size_ = style_.icon_column_width > 0 ? style_.icon_column_width
			: text_height_ + 2;
	if (const char *t = getenv("WB_ICON_THEME"); t && t[0] != '\0')
		icon_theme_ = t;

	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_raise_to_top(&tree_->node);
}

MenuWidget::~MenuWidget() {
	levels_.clear();  /* destroy canvases (scene buffers) before their tree */
	for (auto &[name, surf] : icon_cache_) {
		if (surf != nullptr)
			cairo_surface_destroy(surf);
	}
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
}

/* The icon column width for `menu`: icon size + a small gap when any item has
 * an icon, else zero (so icon-less menus are not indented). */
int MenuWidget::menu_icon_column(const Menu *menu) const {
	if (menu == nullptr)
		return 0;
	for (const MenuItem &item : menu->items) {
		if (!item.icon.empty())
			return icon_size_ + 4;
	}
	return 0;
}

/* Resolve and load an icon (PNG only) once, caching the surface (or nullptr on
 * miss, so we never retry). SVG icons need librsvg and are skipped safely. */
cairo_surface_t *MenuWidget::icon_surface(const std::string &name) {
	if (name.empty())
		return nullptr;
	if (auto it = icon_cache_.find(name); it != icon_cache_.end())
		return it->second;

	const char *home = getenv("HOME");
	const char *xdh = getenv("XDG_DATA_HOME");
	const char *xdd = getenv("XDG_DATA_DIRS");
	std::vector<std::string> bases = icon_base_dirs(home ? home : "",
			xdh ? xdh : "", xdd ? xdd : "");
	cairo_surface_t *loaded = nullptr;
	for (const std::string &path : icon_search_paths(name, icon_theme_,
			icon_size_, bases)) {
		if (path.size() < 4 ||
				path.compare(path.size() - 4, 4, ".png") != 0)
			continue;  /* CPU loader handles PNG only for now */
		cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
		if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
			loaded = s;
			break;
		}
		cairo_surface_destroy(s);
	}
	icon_cache_[name] = loaded;
	return loaded;
}

int MenuWidget::text_width(std::string_view label) const {
	return measure_text(label, style_.item_text.font).width;
}

Rect MenuWidget::output_bounds(int lx, int ly) const {
	struct wlr_output *output = wlr_output_layout_output_at(
			server_->output_layout, lx, ly);
	if (output == nullptr)
		output = wlr_output_layout_output_at(server_->output_layout, 0, 0);
	struct wlr_box box = {};
	if (output != nullptr)
		wlr_output_layout_get_box(server_->output_layout, output, &box);
	if (box.width == 0 || box.height == 0)
		return Rect{0, 0, 1920, 1080};  /* sane fallback for headless */
	return Rect{box.x, box.y, box.width, box.height};
}

float MenuWidget::output_scale(int lx, int ly) const {
	struct wlr_output *output = wlr_output_layout_output_at(
			server_->output_layout, lx, ly);
	return (output != nullptr && output->scale > 0) ? output->scale : 1.0f;
}

void MenuWidget::push_menu(const Menu *menu, const Rect &rect, int parent_item) {
	Level level;
	level.menu = menu;
	level.icon_col = menu_icon_column(menu);
	MenuMetrics m = metrics_;
	m.icon_column = level.icon_col;
	level.layout = layout_menu(*menu, m,
			[this](std::string_view s) { return text_width(s); });
	level.rect = rect;
	level.parent_item = parent_item;
	level.canvas = std::make_unique<SceneCanvas>(tree_, rect.width, rect.height,
			output_scale(rect.x, rect.y));
	level.canvas->set_position(rect.x, rect.y);
	levels_.push_back(std::move(level));
	render_level(levels_.back());
}

bool MenuWidget::open(std::string_view root_id, int lx, int ly) {
	const Menu *menu = menus_.find(root_id);
	if (menu == nullptr)
		return false;
	MenuMetrics m = metrics_;
	m.icon_column = menu_icon_column(menu);
	MenuLayout layout = layout_menu(*menu, m,
			[this](std::string_view s) { return text_width(s); });
	Rect bounds = output_bounds(lx, ly);
	Rect rect = place_root_menu(lx, ly, layout.width, layout.height, bounds);
	push_menu(menu, rect, -1);
	return true;
}

void MenuWidget::render_level(Level &level) {
	cairo_t *cr = level.canvas->cr();
	const Rect &r = level.rect;

	/* Border first (full panel), then the interior fill inset by the border. */
	const Border &b = style_.panel.border;
	if (b.width > 0)
		paint_rect(cr, 0, 0, r.width, r.height, b.color);
	paint_fill(cr, b.width, b.width, r.width - 2 * b.width,
			r.height - 2 * b.width, style_.panel.fill);

	for (size_t i = 0; i < level.menu->items.size() &&
			i < level.layout.item_rects.size(); ++i) {
		const MenuItem &item = level.menu->items[i];
		const Rect &ir = level.layout.item_rects[i];

		if (item.kind == MenuItem::Kind::Separator) {
			int sy = ir.y + ir.height / 2;
			paint_rect(cr, ir.x + metrics_.pad_x, sy,
					ir.width - 2 * metrics_.pad_x, 1, style_.separator);
			continue;
		}

		bool active = static_cast<int>(i) == level.hovered;
		const StateStyle &st = style_.item.for_state(
				active ? WidgetState::Hover : WidgetState::Normal);
		if (active)
			paint_fill(cr, ir.x, ir.y, ir.width, ir.height, st.fill);

		/* Optional icon in the reserved column, vertically centred. */
		if (level.icon_col > 0 && !item.icon.empty()) {
			if (cairo_surface_t *icon = icon_surface(item.icon)) {
				int iw = cairo_image_surface_get_width(icon);
				int ih = cairo_image_surface_get_height(icon);
				if (iw > 0 && ih > 0) {
					double s = static_cast<double>(icon_size_) /
							std::max(iw, ih);
					double dx = ir.x + metrics_.pad_x;
					double dy = ir.y + (ir.height - ih * s) / 2.0;
					cairo_save(cr);
					cairo_translate(cr, dx, dy);
					cairo_scale(cr, s, s);
					cairo_set_source_surface(cr, icon, 0, 0);
					cairo_paint(cr);
					cairo_restore(cr);
				}
			}
		}

		int text_x = ir.x + metrics_.pad_x + level.icon_col;
		int text_y = ir.y + (ir.height - text_height_) / 2;
		paint_text(cr, text_x, text_y, item.label, st.fg,
				style_.item_text.font);

		if (item.kind == MenuItem::Kind::Submenu) {
			/* Right-aligned arrow, its right tip pad_x from the panel edge and
			 * vertically centred on the row (matching the text). */
			int arrow_w = 4;
			double ax = ir.x + ir.width - metrics_.pad_x - arrow_w;
			paint_arrow(cr, ax, ir.y, ir.height, arrow_w, st.fg);
		}
	}

	level.canvas->commit();
}

void MenuWidget::close_below(size_t keep) {
	while (levels_.size() > keep + 1)
		levels_.pop_back();
}

void MenuWidget::open_submenu(size_t level_index, int item_index) {
	Level &parent = levels_[level_index];
	const MenuItem &item = parent.menu->items[item_index];
	const Menu *submenu = menus_.find(item.submenu_id);
	if (submenu == nullptr)
		return;

	/* Already open for this same item? Nothing to do. */
	if (levels_.size() > level_index + 1 &&
			levels_[level_index + 1].parent_item == item_index)
		return;
	close_below(level_index);

	MenuMetrics m = metrics_;
	m.icon_column = menu_icon_column(submenu);
	MenuLayout layout = layout_menu(*submenu, m,
			[this](std::string_view s) { return text_width(s); });
	const Rect &ir = parent.layout.item_rects[item_index];
	Rect bounds = output_bounds(parent.rect.x, parent.rect.y);
	Rect rect = place_submenu(parent.rect, parent.rect.y + ir.y,
			layout.width, layout.height, bounds, style_.panel.border.width);
	push_menu(submenu, rect, item_index);
}

void MenuWidget::on_motion(double lx, double ly) {
	/* Topmost (deepest) level under the pointer wins. */
	for (size_t depth = levels_.size(); depth-- > 0;) {
		Level &level = levels_[depth];
		const Rect &r = level.rect;
		if (lx < r.x || lx >= r.x + r.width || ly < r.y || ly >= r.y + r.height)
			continue;

		int item = menu_item_at(*level.menu, level.layout,
				static_cast<int>(lx) - r.x, static_cast<int>(ly) - r.y);

		/* Pointer is back in a shallower menu: drop deeper submenus. */
		close_below(depth);

		if (level.hovered != item) {
			level.hovered = item;
			render_level(level);
		}
		if (item >= 0 &&
				level.menu->items[item].kind == MenuItem::Kind::Submenu &&
				behavior_.submenu_open == SubmenuOpen::Hover)
			open_submenu(depth, item);
		return;
	}
	/* Pointer is over no panel: leave the menus open (a click outside
	 * dismisses), but clear the root's hover so nothing looks selected. */
}

bool MenuWidget::on_button(uint32_t button, bool pressed) {
	if (!pressed)
		return false;
	double lx = server_->cursor->cursor->x;
	double ly = server_->cursor->cursor->y;

	for (size_t depth = levels_.size(); depth-- > 0;) {
		Level &level = levels_[depth];
		const Rect &r = level.rect;
		if (lx < r.x || lx >= r.x + r.width || ly < r.y || ly >= r.y + r.height)
			continue;
		int item = menu_item_at(*level.menu, level.layout,
				static_cast<int>(lx) - r.x, static_cast<int>(ly) - r.y);
		if (item < 0)
			return false;  /* on padding/border: swallow, keep open */
		const MenuItem &mi = level.menu->items[item];
		if (mi.kind == MenuItem::Kind::Submenu) {
			open_submenu(depth, item);
			return false;
		}
		if (mi.kind == MenuItem::Kind::Entry) {
			pending_ = mi.actions;  /* caller runs these after destroying us */
			return true;  /* dismiss */
		}
		return false;
	}
	(void)button;
	return true;  /* clicked outside every panel: dismiss */
}

bool MenuWidget::on_key(xkb_keysym_t sym) {
	if (levels_.empty())
		return true;
	Level &level = levels_.back();
	const size_t depth = levels_.size() - 1;

	auto reselect = [&](int item) {
		if (level.hovered != item) {
			level.hovered = item;
			render_level(level);
		}
	};

	switch (sym) {
	case XKB_KEY_Escape:
		if (levels_.size() > 1) {
			levels_.pop_back();  /* close the deepest submenu */
			return false;
		}
		return true;  /* close the root: dismiss */

	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		reselect(menu_step_selection(*level.menu, level.hovered, -1,
				behavior_.wrap));
		return false;

	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		reselect(menu_step_selection(*level.menu, level.hovered, +1,
				behavior_.wrap));
		return false;

	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		if (levels_.size() > 1)
			levels_.pop_back();  /* back to the parent menu */
		return false;

	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		/* Open the highlighted submenu and step into it. */
		if (level.hovered >= 0 &&
				level.menu->items[level.hovered].kind ==
						MenuItem::Kind::Submenu) {
			open_submenu(depth, level.hovered);
			if (levels_.size() > depth + 1) {
				Level &child = levels_.back();
				child.hovered = menu_step_selection(*child.menu, -1, +1,
						behavior_.wrap);
				render_level(child);
			}
		}
		return false;

	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (level.hovered < 0)
			return false;
		switch (level.menu->items[level.hovered].kind) {
		case MenuItem::Kind::Submenu:
			open_submenu(depth, level.hovered);
			if (levels_.size() > depth + 1) {
				Level &child = levels_.back();
				child.hovered = menu_step_selection(*child.menu, -1, +1,
						behavior_.wrap);
				render_level(child);
			}
			return false;
		case MenuItem::Kind::Entry:
			pending_ = level.menu->items[level.hovered].actions;
			return true;  /* dismiss; caller runs the actions */
		case MenuItem::Kind::Separator:
			return false;
		}
		return false;

	default:
		return false;
	}
}

}  // namespace wb
