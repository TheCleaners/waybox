/*
 * Resolved presentation/behaviour adapters: turn the raw themerc Theme into the
 * render-ready widget styles, plus the string->enum parsers the config layer
 * uses for waybox extension keys. Pure (no Cairo/wlroots), unit-tested.
 */
#include "waybox/style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
	s.title_text.font = theme.font_menu_header;

	s.item.normal = StateStyle{paint_from_texture(m.items_bg), m.items_text};
	s.item.hover = StateStyle{paint_from_texture(m.items_active_bg), m.items_active_text};
	s.item.pressed = s.item.hover;
	s.item.disabled = StateStyle{paint_from_texture(m.items_bg), m.separator};
	s.item_text.color = m.items_text;
	s.item_text.font = theme.font_menu_item;

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
	f.label.font = active ? theme.font_active_title : theme.font_inactive_title;

	f.handle.fill = paint_from_texture(w.handle_bg);
	f.grip.fill = paint_from_texture(w.grip_bg);
	f.handle_width = theme.handle_width;

	/* Normal button background: when the theme says parentrelative (or sets no
	 * explicit button bg), let the titlebar show through; otherwise use the
	 * themed button bg. Hover uses the accent (hover) bg, falling back to the
	 * titlebar's active gradient so a bare theme still shows feedback. */
	Paint normal_btn = w.button_bg_parentrelative
			? paint_from_texture(w.title_bg)
			: paint_from_texture(w.button_bg);
	bool has_hover_bg = w.button_hover_bg.color.a != 0 ||
			w.button_hover_bg.color_to.a != 0;
	Paint hover_btn = has_hover_bg ? paint_from_texture(w.button_hover_bg)
			: normal_btn;
	Color icon_hover = (w.button_icon_hover.a != 0) ? w.button_icon_hover
			: w.button_icon;

	/* Pressed (held-down): the theme's pressed bg if set, else a slightly
	 * darker shade derived from the hover accent so there is feedback. */
	auto darken = [](Color c, double f) {
		c.r = static_cast<uint8_t>(c.r * f);
		c.g = static_cast<uint8_t>(c.g * f);
		c.b = static_cast<uint8_t>(c.b * f);
		return c;
	};
	bool has_pressed_bg = w.button_pressed_bg.color.a != 0 ||
			w.button_pressed_bg.color_to.a != 0;
	Paint pressed_btn;
	if (has_pressed_bg) {
		pressed_btn = paint_from_texture(w.button_pressed_bg);
	} else if (has_hover_bg) {
		Texture d = w.button_hover_bg;
		d.color = darken(d.color, 0.7);
		d.color_to = darken(d.color_to, 0.7);
		pressed_btn = paint_from_texture(d);
	} else {
		pressed_btn = hover_btn;
	}
	Color icon_pressed = (w.button_icon_pressed.a != 0) ? w.button_icon_pressed
			: icon_hover;

	f.button.normal = StateStyle{normal_btn, w.button_icon};
	f.button.hover = StateStyle{hover_btn, icon_hover};
	f.button.pressed = StateStyle{pressed_btn, icon_pressed};
	f.button.disabled = StateStyle{normal_btn, w.button_icon_disabled};
	return f;
}

SwitcherStyle switcher_style_from_theme(const Theme &theme) {
	const MenuColors &m = theme.menu;
	const OsdColors &o = theme.osd;
	SwitcherStyle s;
	/* Openbox's osd.* keys where the theme has them, else the menu's. */
	Texture bg = o.bg_set ? o.bg : m.items_bg;
	Texture hilight = o.hilight_set ? o.hilight_bg : m.items_active_bg;
	Color text = o.text_set ? o.text : m.items_text;
	Color active_text = o.active_text_set ? o.active_text
			: o.text_set ? o.text : m.items_active_text;

	s.panel.fill = paint_from_texture(bg);
	s.panel.border.width = o.border_width >= 0 ? o.border_width : theme.border_width;
	s.panel.border.color = o.border_color_set ? o.border_color : theme.border_color;
	s.panel.border.radius = theme.menu_corner_radius;
	s.panel.padding = Insets{theme.padding_y, theme.padding_x,
			theme.padding_y, theme.padding_x};
	s.panel.opacity = theme.menu_opacity;

	s.item.normal = StateStyle{paint_from_texture(bg), text};
	s.item.hover = StateStyle{paint_from_texture(hilight), active_text};
	s.item.pressed = s.item.hover;
	s.item.disabled = StateStyle{paint_from_texture(bg), m.separator};
	s.item_text.color = text;
	s.item_text.font = theme.font_osd;
	s.pager_on = paint_from_texture(hilight);
	s.pager_off = paint_from_texture(o.unhilight_set ? o.unhilight_bg : bg);
	return s;
}

SwitcherStyle osd_style(const Theme &theme, const OsdConfig &c) {
	SwitcherStyle s = switcher_style_from_theme(theme);
	s.position = c.position;
	if (c.bg) {
		Texture t;
		t.color = t.color_to = *c.bg;
		s.panel.fill = paint_from_texture(t);
		s.item.normal.fill = s.panel.fill;
		s.item.disabled.fill = s.panel.fill;
		if (!theme.osd.unhilight_set)
			s.pager_off = s.panel.fill;
	}
	if (c.text) {
		s.item_text.color = *c.text;
		s.item.normal.fg = *c.text;
	}
	if (c.border) {
		s.panel.border.color = *c.border;
		if (s.panel.border.width == 0)
			s.panel.border.width = 1;  /* a colour asked for: shown */
	}
	if (c.corner_radius)
		s.panel.border.radius = *c.corner_radius;
	if (c.opacity)
		s.panel.opacity = *c.opacity / 100.0;
	return s;
}

std::optional<OsdPosition> osd_position_from_name(std::string_view name) {
	if (name == "center" || name == "centre")
		return OsdPosition::Center;
	if (name == "top")
		return OsdPosition::Top;
	if (name == "bottom")
		return OsdPosition::Bottom;
	return std::nullopt;
}

OsdConfig parse_osd_config(std::string_view position, std::string_view opacity,
		std::string_view corner_radius, std::string_view bg, std::string_view text,
		std::string_view border, std::vector<std::string> *problems) {
	OsdConfig c;
	auto problem = [problems](const char *key, std::string_view v) {
		if (problems != nullptr)
			problems->push_back(std::string(key) + "=" + std::string(v.substr(0, 64)));
	};
	if (!position.empty()) {
		if (auto p = osd_position_from_name(position))
			c.position = *p;
		else
			problem("position", position);
	}
	auto number = [&](const char *key, std::string_view v, int lo, int hi)
			-> std::optional<int> {
		if (v.empty())
			return std::nullopt;
		std::string s(v);
		char *end = nullptr;
		long n = strtol(s.c_str(), &end, 10);
		if (end == s.c_str() || *end != '\0' || n < lo || n > hi) {
			problem(key, v);
			return std::nullopt;
		}
		return static_cast<int>(n);
	};
	c.opacity = number("opacity", opacity, 10, 100);
	c.corner_radius = number("cornerRadius", corner_radius, 0, 64);
	auto color = [&](const char *key, std::string_view v) -> std::optional<Color> {
		if (v.empty())
			return std::nullopt;
		if (auto col = parse_color(v))
			return col;
		problem(key, v);
		return std::nullopt;
	};
	c.bg = color("bg", bg);
	c.text = color("text", text);
	c.border = color("border", border);
	return c;
}

int osd_y(OsdPosition position, int box_y, int box_h, int h) {
	int room = box_h - h;
	if (room <= 0)
		return box_y;  /* taller than the monitor: its top at the top */
	int margin = box_h / 10;
	if (margin > room)
		margin = room;
	switch (position) {
	case OsdPosition::Top:
		return box_y + margin;
	case OsdPosition::Bottom:
		return box_y + box_h - h - margin;
	case OsdPosition::Center:
	default:
		return box_y + room / 2;
	}
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
