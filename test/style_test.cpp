#include "wb_test.hpp"

#include "waybox/style.hpp"
#include "waybox/theme.hpp"

using wb::Color;
using wb::Justify;
using wb::SubmenuOpen;
using wb::SwitcherOrder;
using wb::Theme;

WB_TEST(menu_style_maps_themerc_colours) {
	Theme t = wb::default_theme();
	wb::MenuStyle s = wb::menu_style_from_theme(t);

	/* panel fill comes from menu items bg; border from the theme border */
	WB_CHECK(wb::solid_color(s.panel.fill) == t.menu.items_bg.color);
	WB_CHECK(s.panel.border.width == t.border_width);
	/* hover (active) item styling comes from the active menu colours */
	WB_CHECK(wb::solid_color(s.item.hover.fill) == t.menu.items_active_bg.color);
	WB_CHECK(s.item.hover.fg == t.menu.items_active_text);
	WB_CHECK(s.item.normal.fg == t.menu.items_text);
	WB_CHECK(s.separator == t.menu.separator);
}

WB_TEST(menu_style_carries_waybox_extensions) {
	Theme t = wb::parse_themerc(
			"waybox.menu.corner.radius: 6\n"
			"waybox.menu.item.spacing: 3\n"
			"waybox.menu.opacity: 0.85\n");
	wb::MenuStyle s = wb::menu_style_from_theme(t);
	WB_CHECK(s.panel.border.radius == 6);
	WB_CHECK(s.item_spacing == 3);
	WB_CHECK(s.panel.opacity > 0.84 && s.panel.opacity < 0.86);
}

WB_TEST(menu_opacity_clamps) {
	WB_CHECK(wb::parse_themerc("waybox.menu.opacity: 2.0\n").menu_opacity == 1.0);
	WB_CHECK(wb::parse_themerc("waybox.menu.opacity: -1\n").menu_opacity == 0.0);
}

WB_TEST(menu_metrics_bridge_to_layout) {
	Theme t = wb::default_theme();
	wb::MenuStyle s = wb::menu_style_from_theme(t);
	s.item_height = 0;  /* force derivation from the font */
	wb::MenuMetrics m = wb::menu_metrics(s);
	WB_CHECK(m.item_height > 0);          /* derived a sane row height */
	WB_CHECK(m.border == s.panel.border.width);
	WB_CHECK(m.pad_x == s.panel.padding.left);
	WB_CHECK(m.submenu_arrow_width == s.submenu_arrow_width);

	s.item_height = 24;                   /* explicit height wins */
	WB_CHECK(wb::menu_metrics(s).item_height == 24);
}

WB_TEST(frame_style_distinguishes_active_inactive) {
	Theme t = wb::parse_themerc(
			"window.active.border.color: #112233\n"
			"window.inactive.border.color: #445566\n"
			"window.active.label.text.color: #ffffff\n"
			"window.handle.width: 5\n"
			"window.label.text.justify: center\n");
	wb::FrameStyle active = wb::frame_style_from_theme(t, true);
	wb::FrameStyle inactive = wb::frame_style_from_theme(t, false);

	WB_CHECK(active.border.color == (Color{0x11, 0x22, 0x33, 255}));
	WB_CHECK(inactive.border.color == (Color{0x44, 0x55, 0x66, 255}));
	WB_CHECK(active.label.color == (Color{255, 255, 255, 255}));
	WB_CHECK(active.handle_width == 5);
	WB_CHECK(active.label.justify == Justify::Center);
}

WB_TEST(frame_button_states_map) {
	Theme t = wb::parse_themerc(
			"window.active.button.unpressed.image.color: #101010\n"
			"window.active.button.hover.image.color: #202020\n"
			"window.active.button.pressed.image.color: #303030\n");
	wb::FrameStyle f = wb::frame_style_from_theme(t, true);
	WB_CHECK(f.button.normal.fg == (Color{0x10, 0x10, 0x10, 255}));
	WB_CHECK(f.button.hover.fg == (Color{0x20, 0x20, 0x20, 255}));
	WB_CHECK(f.button.pressed.fg == (Color{0x30, 0x30, 0x30, 255}));
}

WB_TEST(switcher_style_from_theme_is_usable) {
	Theme t = wb::default_theme();
	wb::SwitcherStyle s = wb::switcher_style_from_theme(t);
	WB_CHECK(wb::solid_color(s.panel.fill) == t.menu.items_bg.color);
	WB_CHECK(wb::solid_color(s.item.hover.fill) == t.menu.items_active_bg.color);
	WB_CHECK(s.orientation == wb::SwitcherOrientation::Vertical);
}

WB_TEST(enum_parsers_round_trip_names) {
	WB_CHECK(wb::submenu_open_from_name("hover") == SubmenuOpen::Hover);
	WB_CHECK(wb::submenu_open_from_name("click") == SubmenuOpen::Click);
	WB_CHECK(!wb::submenu_open_from_name("bogus").has_value());

	WB_CHECK(wb::switcher_order_from_name("mru") == SwitcherOrder::MRU);
	WB_CHECK(wb::switcher_order_from_name("stacking") == SwitcherOrder::Stacking);
	WB_CHECK(wb::switcher_order_from_name("spatial") == SwitcherOrder::Spatial);
	WB_CHECK(!wb::switcher_order_from_name("bogus").has_value());

	WB_CHECK(wb::justify_from_name("left") == Justify::Left);
	WB_CHECK(wb::justify_from_name("centre") == Justify::Center);
	WB_CHECK(wb::justify_from_name("right") == Justify::Right);
	WB_CHECK(!wb::justify_from_name("bogus").has_value());
}

WB_TEST(theme_fonts_flow_into_resolved_styles) {
	wb::Theme t = wb::default_theme();
	t.font_active_title = {"Inter", 13, true};
	t.font_inactive_title = {"Inter", 11, false};
	t.font_menu_item = {"Cantarell", 12, false};
	t.font_osd = {"Mono", 9, false};

	wb::FrameStyle fa = wb::frame_style_from_theme(t, true);
	WB_CHECK(fa.label.font.family == "Inter");
	WB_CHECK(fa.label.font.size_pt == 13 && fa.label.font.bold == true);
	wb::FrameStyle fi = wb::frame_style_from_theme(t, false);
	WB_CHECK(fi.label.font.size_pt == 11 && fi.label.font.bold == false);

	wb::MenuStyle ms = wb::menu_style_from_theme(t);
	WB_CHECK(ms.item_text.font.family == "Cantarell");
	wb::SwitcherStyle ss = wb::switcher_style_from_theme(t);
	WB_CHECK(ss.item_text.font.family == "Mono" && ss.item_text.font.size_pt == 9);
}
