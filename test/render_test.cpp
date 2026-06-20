#include "wb_test.hpp"

#include "waybox/render.hpp"

#include <cairo.h>

namespace {

/* Read a pixel from an ARGB32 cairo surface as (r,g,b,a), un-premultiplied
 * enough for solid-colour checks. */
struct Px {
	int r, g, b, a;
};

Px pixel_at(cairo_surface_t *s, int x, int y) {
	cairo_surface_flush(s);
	const unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
	uint32_t p = row[x];  /* CAIRO_FORMAT_ARGB32: 0xAARRGGBB (native endian) */
	int a = (p >> 24) & 0xff;
	int r = (p >> 16) & 0xff;
	int g = (p >> 8) & 0xff;
	int b = p & 0xff;
	return Px{r, g, b, a};
}

cairo_t *make(cairo_surface_t **out, int w, int h) {
	*out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	return cairo_create(*out);
}

}  // namespace

WB_TEST(paint_rect_fills_solid_color) {
	cairo_surface_t *s;
	cairo_t *cr = make(&s, 20, 20);
	wb::paint_rect(cr, 0, 0, 20, 20, wb::Color{200, 100, 50, 255});
	Px p = pixel_at(s, 10, 10);
	WB_CHECK(p.r == 200 && p.g == 100 && p.b == 50 && p.a == 255);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

WB_TEST(paint_rect_only_covers_its_area) {
	cairo_surface_t *s;
	cairo_t *cr = make(&s, 40, 40);
	wb::paint_rect(cr, 10, 10, 10, 10, wb::Color{255, 0, 0, 255});
	WB_CHECK(pixel_at(s, 15, 15).r == 255);  /* inside */
	WB_CHECK(pixel_at(s, 0, 0).a == 0);      /* outside stays transparent */
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

WB_TEST(paint_texture_gradient_varies_top_to_bottom) {
	cairo_surface_t *s;
	cairo_t *cr = make(&s, 10, 100);
	wb::Texture tex;
	tex.type = wb::TextureType::Gradient;
	tex.color = wb::Color{0, 0, 0, 255};
	tex.color_to = wb::Color{255, 255, 255, 255};
	wb::paint_texture(cr, 0, 0, 10, 100, tex);
	Px top = pixel_at(s, 5, 2);
	Px bottom = pixel_at(s, 5, 97);
	WB_CHECK(top.r < 64);        /* near black at the top */
	WB_CHECK(bottom.r > 192);    /* near white at the bottom */
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

WB_TEST(measure_text_is_positive_for_nonempty) {
	wb::TextExtents e = wb::measure_text("Waybox", wb::FontSpec{});
	WB_CHECK(e.width > 0);
	WB_CHECK(e.height > 0);
	/* a longer string is wider */
	wb::TextExtents longer = wb::measure_text("Waybox Waybox", wb::FontSpec{});
	WB_CHECK(longer.width > e.width);
}

WB_TEST(paint_text_puts_ink_on_the_surface) {
	cairo_surface_t *s;
	cairo_t *cr = make(&s, 120, 30);
	wb::paint_text(cr, 2, 2, "Hi", wb::Color{255, 255, 255, 255}, wb::FontSpec{});
	/* drawn glyphs leave non-zero alpha somewhere on the surface (requires a
	 * usable font to be installed; CI provisions one) */
	bool ink = false;
	for (int x = 0; x < 120 && !ink; ++x)
		for (int y = 0; y < 30 && !ink; ++y)
			if (pixel_at(s, x, y).a > 0)
				ink = true;
	WB_CHECK(ink);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}
