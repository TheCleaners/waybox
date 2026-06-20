#include "wb_test.hpp"

#include "waybox/theme.hpp"

#include <string>

using wb::Color;
using wb::parse_color;
using wb::TextureType;
using wb::Theme;

WB_TEST(color_hex_forms) {
	WB_CHECK(parse_color("#ffffff") == Color{255, 255, 255, 255});
	WB_CHECK(parse_color("#000000") == Color{0, 0, 0, 255});
	WB_CHECK(parse_color("#ff8800") == Color{255, 136, 0, 255});
	WB_CHECK(parse_color("#f80") == Color{255, 136, 0, 255});  /* #rgb expands */
	WB_CHECK(parse_color("#11223344") == Color{0x11, 0x22, 0x33, 0x44});
}

WB_TEST(color_x11_and_named) {
	WB_CHECK(parse_color("rgb:ff/88/00") == Color{255, 136, 0, 255});
	WB_CHECK(parse_color("white") == Color{255, 255, 255, 255});
	WB_CHECK(parse_color("Black") == Color{0, 0, 0, 255});  /* case-insensitive */
	WB_CHECK(parse_color("  #ff8800  ").has_value());        /* trims */
}

WB_TEST(color_rejects_garbage) {
	WB_CHECK(!parse_color("").has_value());
	WB_CHECK(!parse_color("#xyz").has_value());
	WB_CHECK(!parse_color("#12345").has_value());   /* bad length */
	WB_CHECK(!parse_color("notacolor").has_value());
	WB_CHECK(!parse_color("rgb:zz/00/00").has_value());
}

WB_TEST(default_theme_is_usable) {
	Theme t = wb::default_theme();
	WB_CHECK(t.border_width >= 0);
	/* active title differs from inactive, so windows are distinguishable */
	WB_CHECK(t.window_active.title_bg.color != t.window_inactive.title_bg.color);
}

WB_TEST(themerc_sets_known_keys) {
	Theme t = wb::parse_themerc(
			"! a comment\n"
			"window.active.title.bg.color: #102030\n"
			"window.active.label.text.color: #ffffff\n"
			"menu.items.bg.color: #e0e0e0\n"
			"menu.items.active.bg.color: #305080\n"
			"menu.items.active.text.color: white\n"
			"menu.separator.color: #808080\n"
			"border.width: 2\n"
			"menu.overlap.x: 5\n");
	WB_CHECK(t.window_active.title_bg.color == (Color{0x10, 0x20, 0x30, 255}));
	WB_CHECK(t.window_active.label_text == (Color{255, 255, 255, 255}));
	WB_CHECK(t.menu.items_bg.color == (Color{0xe0, 0xe0, 0xe0, 255}));
	WB_CHECK(t.menu.items_active_bg.color == (Color{0x30, 0x50, 0x80, 255}));
	WB_CHECK(t.menu.items_active_text == (Color{255, 255, 255, 255}));
	WB_CHECK(t.border_width == 2);
	WB_CHECK(t.menu_overlap_x == 5);
}

WB_TEST(themerc_detects_gradient_texture) {
	Theme t = wb::parse_themerc(
			"window.active.title.bg: raised gradient vertical\n"
			"window.active.title.bg.color: #305080\n"
			"window.active.title.bg.colorTo: #102040\n");
	WB_CHECK(t.window_active.title_bg.type == TextureType::Gradient);
	WB_CHECK(t.window_active.title_bg.color == (Color{0x30, 0x50, 0x80, 255}));
	WB_CHECK(t.window_active.title_bg.color_to == (Color{0x10, 0x20, 0x40, 255}));
}

WB_TEST(alpha_via_eight_digit_hex) {
	/* the #rrggbbaa form carries alpha through the model */
	Theme t = wb::parse_themerc("menu.items.bg.color: #102030c0\n");
	WB_CHECK(t.menu.items_bg.color == (Color{0x10, 0x20, 0x30, 0xc0}));
}

WB_TEST(alpha_suffix_extension_order_independent) {
	/* the waybox ".alpha" extension sets alpha on any colour, even rgb:/named,
	 * regardless of whether it appears before or after the colour key */
	Theme after = wb::parse_themerc(
			"menu.items.bg.color: rgb:10/20/30\n"
			"menu.items.bg.color.alpha: 128\n");
	WB_CHECK(after.menu.items_bg.color == (Color{0x10, 0x20, 0x30, 128}));

	Theme before = wb::parse_themerc(
			"menu.items.bg.color.alpha: 200\n"
			"menu.items.bg.color: #405060\n");
	WB_CHECK(before.menu.items_bg.color == (Color{0x40, 0x50, 0x60, 200}));
}

WB_TEST(alpha_suffix_clamps_and_defaults_opaque) {
	WB_CHECK(wb::parse_themerc("border.color.alpha: 999\n").border_color.a == 255);
	WB_CHECK(wb::parse_themerc("border.color.alpha: -5\n").border_color.a == 0);
	/* colours with no alpha specified stay fully opaque */
	WB_CHECK(wb::parse_themerc("menu.items.text.color: #abcdef\n").menu.items_text.a == 255);
}

WB_TEST(themerc_ignores_unknown_and_malformed) {
	Theme base = wb::default_theme();
	Theme t = wb::parse_themerc(
			"this line has no colon\n"
			"some.unknown.key: #ffffff\n"
			"menu.items.text.color: #abcdef\n"
			"window.active.title.bg.color: not-a-color\n");  /* bad value ignored */
	/* the one good key applied... */
	WB_CHECK(t.menu.items_text == (Color{0xab, 0xcd, 0xef, 255}));
	/* ...the bad-value key kept the default */
	WB_CHECK(t.window_active.title_bg.color == base.window_active.title_bg.color);
}

WB_TEST(themerc_parses_full_window_section) {
	Theme t = wb::parse_themerc(
			"window.active.border.color: #112233\n"
			"window.inactive.border.color: #445566\n"
			"window.active.client.color: #778899\n"
			"window.active.handle.bg: flat solid\n"
			"window.active.handle.bg.color: #abcdef\n"
			"window.active.grip.bg.color: #fedcba\n"
			"window.active.button.unpressed.image.color: #010203\n"
			"window.active.button.hover.image.color: #040506\n"
			"window.handle.width: 6\n"
			"window.label.text.justify: right\n");
	WB_CHECK(t.window_active.border_color == (Color{0x11, 0x22, 0x33, 255}));
	WB_CHECK(t.window_inactive.border_color == (Color{0x44, 0x55, 0x66, 255}));
	WB_CHECK(t.window_active.client_color == (Color{0x77, 0x88, 0x99, 255}));
	WB_CHECK(t.window_active.handle_bg.color == (Color{0xab, 0xcd, 0xef, 255}));
	WB_CHECK(t.window_active.grip_bg.color == (Color{0xfe, 0xdc, 0xba, 255}));
	WB_CHECK(t.window_active.button_icon == (Color{0x01, 0x02, 0x03, 255}));
	WB_CHECK(t.window_active.button_icon_hover == (Color{0x04, 0x05, 0x06, 255}));
	WB_CHECK(t.handle_width == 6);
	WB_CHECK(t.label_justify == wb::Justify::Right);
}

WB_TEST(themerc_waybox_extension_keys) {
	Theme t = wb::parse_themerc(
			"waybox.menu.corner.radius: 8\n"
			"waybox.window.corner.radius: 4\n"
			"waybox.menu.item.spacing: 2\n");
	WB_CHECK(t.menu_corner_radius == 8);
	WB_CHECK(t.window_corner_radius == 4);
	WB_CHECK(t.menu_item_spacing == 2);
}

WB_TEST(search_paths_follow_openbox_convention) {
	auto paths = wb::themerc_search_paths("Clearlooks", "/home/u",
			"/home/u/.local/share", "/usr/local/share:/usr/share");
	/* XDG_DATA_HOME first, then ~/.themes, then each XDG_DATA_DIRS entry */
	WB_CHECK(paths.size() == 4);
	WB_CHECK(paths[0] == "/home/u/.local/share/themes/Clearlooks/openbox-3/themerc");
	WB_CHECK(paths[1] == "/home/u/.themes/Clearlooks/openbox-3/themerc");
	WB_CHECK(paths[2] == "/usr/local/share/themes/Clearlooks/openbox-3/themerc");
	WB_CHECK(paths[3] == "/usr/share/themes/Clearlooks/openbox-3/themerc");
}

WB_TEST(search_paths_default_data_dirs_and_no_xdg_home) {
	auto paths = wb::themerc_search_paths("Onyx", "/home/u", "", "");
	/* falls back to ~/.local/share/themes, ~/.themes, and the default dirs */
	WB_CHECK(paths.size() == 4);
	WB_CHECK(paths[0] == "/home/u/.local/share/themes/Onyx/openbox-3/themerc");
	WB_CHECK(paths[3] == "/usr/share/themes/Onyx/openbox-3/themerc");
}
