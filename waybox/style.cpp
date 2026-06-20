/*
 * Resolved presentation/behaviour adapters: turn the raw themerc Theme into the
 * render-ready widget styles, plus the string->enum parsers the config layer
 * uses for waybox extension keys. Pure (no Cairo/wlroots), unit-tested.
 */
#include "waybox/style.hpp"

#include <algorithm>
#include <cmath>

namespace wb {

namespace {

/* Approximate a comfortable row height from a label font when the theme does
 * not pin one: text cell (pt->px) plus vertical breathing room. */
int derived_item_height(const FontSpec &font, int spacing) {
	int text_px = static_cast<int>(std::lround(font.size_pt * 4.0 / 3.0));
	return text_px + 8 + spacing;
}

}  // namespace

MenuStyle menu_style_from_theme(const Theme &theme) {
	const MenuColors &m = theme.menu;
	MenuStyle s;

	s.panel.fill = paint_from_texture(m.items_bg);
	s.panel.border.width = theme.border_width;
	s.panel.border.color = theme.border_color;
	s.panel.border.radius = theme.menu_corner_radius;
	s.panel.padding = Insets{theme.padding_y, theme.padding_x,
			theme.padding_y, theme.padding_x};
	s.panel.opacity = theme.menu_opacity;

	s.title_bar.fill = paint_from_texture(m.title_bg);
	s.title_text.color = m.title_text;

	s.item.normal = StateStyle{paint_from_texture(m.items_bg), m.items_text};
	s.item.hover = StateStyle{paint_from_texture(m.items_active_bg), m.items_active_text};
	s.item.pressed = s.item.hover;
	s.item.disabled = StateStyle{paint_from_texture(m.items_bg), m.separator};
	s.item_text.color = m.items_text;

	s.separator = m.separator;
	s.item_spacing = theme.menu_item_spacing;
	return s;
}

FrameStyle frame_style_from_theme(const Theme &theme, bool active) {
	const WindowColors &w = active ? theme.window_active : theme.window_inactive;
	FrameStyle f;

	f.border.width = theme.border_width;
	f.border.color = w.border_color;
	f.border.radius = theme.window_corner_radius;
	f.corner_radius = theme.window_corner_radius;
	f.client_line = w.client_color;

	f.title_bar.fill = paint_from_texture(w.title_bg);
	f.title_bar.padding = Insets{theme.padding_y, theme.padding_x,
			theme.padding_y, theme.padding_x};
	f.label.color = w.label_text;
	f.label.justify = theme.label_justify;

	f.handle.fill = paint_from_texture(w.handle_bg);
	f.grip.fill = paint_from_texture(w.grip_bg);
	f.handle_width = theme.handle_width;

	f.button.normal = StateStyle{paint_from_texture(w.button_bg), w.button_icon};
	f.button.hover = StateStyle{paint_from_texture(w.button_bg), w.button_icon_hover};
	f.button.pressed = StateStyle{paint_from_texture(w.button_bg), w.button_icon_pressed};
	f.button.disabled = StateStyle{paint_from_texture(w.button_bg), w.button_icon_disabled};
	return f;
}

SwitcherStyle switcher_style_from_theme(const Theme &theme) {
	const MenuColors &m = theme.menu;
	SwitcherStyle s;

	s.panel.fill = paint_from_texture(m.items_bg);
	s.panel.border.width = theme.border_width;
	s.panel.border.color = theme.border_color;
	s.panel.border.radius = theme.menu_corner_radius;
	s.panel.padding = Insets{theme.padding_y, theme.padding_x,
			theme.padding_y, theme.padding_x};
	s.panel.opacity = theme.menu_opacity;

	s.item.normal = StateStyle{paint_from_texture(m.items_bg), m.items_text};
	s.item.hover = StateStyle{paint_from_texture(m.items_active_bg), m.items_active_text};
	s.item.pressed = s.item.hover;
	s.item.disabled = StateStyle{paint_from_texture(m.items_bg), m.separator};
	s.item_text.color = m.items_text;
	return s;
}

MenuMetrics menu_metrics(const MenuStyle &style) {
	MenuMetrics m;
	m.item_height = style.item_height > 0
			? style.item_height
			: derived_item_height(style.item_text.font, style.item_spacing);
	m.separator_height = style.separator_height;
	m.pad_x = style.panel.padding.left;
	m.pad_y = style.panel.padding.top;
	m.border = style.panel.border.width;
	m.min_width = style.min_width;
	m.submenu_arrow_width = style.submenu_arrow_width;
	return m;
}

std::optional<SubmenuOpen> submenu_open_from_name(std::string_view name) {
	if (name == "hover")
		return SubmenuOpen::Hover;
	if (name == "click")
		return SubmenuOpen::Click;
	return std::nullopt;
}

std::optional<SwitcherOrder> switcher_order_from_name(std::string_view name) {
	if (name == "mru")
		return SwitcherOrder::MRU;
	if (name == "stacking")
		return SwitcherOrder::Stacking;
	if (name == "spatial")
		return SwitcherOrder::Spatial;
	return std::nullopt;
}

}  // namespace wb
