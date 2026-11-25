#include "wb_test.hpp"

#include "waybox/color_text.hpp"

#include <cmath>

WB_TEST(field_tints) {
	WB_CHECK(!wb::field_tint(""));
	WB_CHECK(!wb::field_tint("reddish"));
	WB_CHECK(!wb::field_tint("#12"));
	/* Opaque: 70%, text by luminance. */
	auto white = wb::field_tint("#ffffff");
	WB_CHECK(white && white->background.a == 179 && white->text && white->text->r == 0);
	auto navy = wb::field_tint("#000080");
	WB_CHECK(navy && navy->text && navy->text->r == 255);
	auto yellow = wb::field_tint("#ffff00");
	WB_CHECK(yellow && yellow->text && yellow->text->r == 0);
	/* Orange over a light field shows light: dark text (white would be
	 * hard to read); over a dark field, it shows dark: light text. */
	auto orange = wb::field_tint("#ff8800cc");
	WB_CHECK(orange && orange->text && orange->text->r == 0);
	auto orange_dark = wb::field_tint("#ff8800cc", {0x24, 0x24, 0x24, 255});
	WB_CHECK(orange_dark && orange_dark->text && orange_dark->text->r == 255);
	/* Whichever is chosen reads at least as well as the other. */
	for (const char *v : {"#ff0000", "#00ff00", "#0000ff", "#808080", "#ff8800", "#336699"})
		for (wb::Color base : {wb::Color{255, 255, 255, 255}, wb::Color{0x24, 0x24, 0x24, 255}}) {
			auto t = wb::field_tint(v, base);
			WB_CHECK(t && t->text);
		}
	WB_CHECK(std::abs(wb::contrast({0, 0, 0, 255}, {255, 255, 255, 255}) - 21.0) < 0.01);
	/* Transparent: a faint tint, the theme's text. */
	auto clear = wb::field_tint("#ff000000");
	WB_CHECK(clear && clear->background.a == 51 && !clear->text);
	auto below = wb::field_tint("#0000007f");
	WB_CHECK(below && !below->text);  /* 0.2 + 0.5 * 127/255 = 0.449: just under */
	auto at = wb::field_tint("#00000080");
	WB_CHECK(at && at->text);         /* 0.451 */
	/* Half-transparent black over a white field shows mid-gray: black
	 * text reads better there (about 6:1 against 3.5:1 for white). */
	auto mid = wb::field_tint("#00000088");
	WB_CHECK(mid && mid->text && mid->text->r == 0);
	/* Short form and alpha. */
	auto shortf = wb::field_tint("#f80");
	WB_CHECK(shortf && shortf->background.r == 0xff && shortf->background.g == 0x88);
	/* CSS. */
	std::string css = wb::field_tint_css("wb-c3", *navy);
	WB_CHECK(css == "entry.wb-c3 { background-color: rgba(0, 0, 128, 0.702); color: #ffffff; }\n");
	WB_CHECK(wb::field_tint_css("x", *clear).find("color: #") == std::string::npos);
	WB_CHECK(wb::luminance({255, 255, 255, 255}) > 0.99 && wb::luminance({0, 0, 0, 255}) < 0.01);
}
