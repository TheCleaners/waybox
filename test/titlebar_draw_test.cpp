#include "wb_test.hpp"

#include "waybox/style.hpp"
#include "waybox/theme.hpp"
#include "waybox/titlebar_draw.hpp"

#include <cairo.h>

namespace {
/* Whether anything other than the titlebar fill was drawn in `r`. */
bool drawn_in(cairo_surface_t *s, const wb::Rect &r, uint32_t fill) {
	cairo_surface_flush(s);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int y = r.y; y < r.y + r.height; ++y)
		for (int x = r.x; x < r.x + r.width; ++x)
			if (reinterpret_cast<uint32_t *>(data + y * stride)[x] != fill)
				return true;
	return false;
}
}  // namespace

WB_TEST(metrics_follow_the_titlebar_settings) {
	wb::Theme theme = wb::default_theme();
	wb::FrameStyle fs = wb::frame_style_from_theme(theme, true);
	wb::FrameMetrics automatic = wb::frame_metrics_for(theme, fs, -1, 0, -1);
	wb::FrameMetrics set = wb::frame_metrics_for(theme, fs, 8, 30, 4);
	WB_CHECK(set.titlebar == automatic.titlebar - 2 * std::max(theme.padding_y - 1, 1) + 16);
	WB_CHECK(set.button == 30 && set.resize_grab == 4);
	WB_CHECK(automatic.button == automatic.titlebar * 2 / 3 || automatic.button == 10);
}

WB_TEST(titlebar_draws_buttons_where_the_layout_puts_them) {
	wb::Theme theme = wb::default_theme();
	wb::FrameStyle fs = wb::frame_style_from_theme(theme, true);
	/* A flat fill so "drawn" is easy to tell. */
	fs.title_bar.fill = wb::SolidPaint{wb::Color{10, 20, 30, 255}};
	uint32_t fill = 0xff0a141e;
	wb::FrameMetrics m = wb::frame_metrics_for(theme, fs, -1, 0, -1);
	int outer_w = 300;
	wb::TitleLayout layout = wb::parse_title_layout("C:L");  /* close left */
	if (layout.buttons.empty())
		layout = wb::parse_title_layout("CL");
	wb::Rect tb = wb::titlebar_rect(outer_w, m);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tb.width, tb.height);
	cairo_t *cr = cairo_create(s);
	wb::TitlebarState st;  /* no title: only the buttons draw */
	wb::draw_titlebar(cr, outer_w, m, fs, layout.buttons, layout.left, st);
	std::vector<wb::Rect> rects = wb::button_rects(outer_w, m, layout.buttons, layout.left);
	WB_CHECK(rects.size() == 1);
	wb::Rect close = rects[0];
	close.x -= tb.x;
	close.y -= tb.y;
	WB_CHECK(close.x < tb.width / 2);             /* on the left */
	WB_CHECK(drawn_in(s, close, fill));           /* the close glyph is there */
	wb::Rect right{tb.width - close.width - 4, close.y, close.width, close.height};
	WB_CHECK(!drawn_in(s, right, fill));          /* nothing on the right */
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}
