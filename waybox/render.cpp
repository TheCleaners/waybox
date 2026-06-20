/*
 * Cairo/Pango painting primitives. Depends on cairo + pango (+ glib via pango),
 * but NOT on wlroots, so it links into the standalone pixel test.
 */
#include "waybox/render.hpp"

#include <string>

#include <pango/pangocairo.h>

namespace wb {

namespace {

void set_source(cairo_t *cr, const Color &c) {
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
			c.a / 255.0);
}

PangoLayout *make_layout(cairo_t *cr, std::string_view text,
		const FontSpec &font) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_new();
	pango_font_description_set_family(desc, font.family);
	pango_font_description_set_weight(desc,
			font.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_font_description_set_size(desc, font.size_pt * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	const std::string owned(text);
	pango_layout_set_text(layout, owned.c_str(), static_cast<int>(owned.size()));
	return layout;
}

}  // namespace

TextExtents measure_text(std::string_view text, const FontSpec &font) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(surface);
	PangoLayout *layout = make_layout(cr, text, font);

	int w = 0;
	int h = 0;
	pango_layout_get_pixel_size(layout, &w, &h);

	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return TextExtents{w, h};
}

void paint_rect(cairo_t *cr, double x, double y, double w, double h,
		const Color &color) {
	set_source(cr, color);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

void paint_texture(cairo_t *cr, double x, double y, double w, double h,
		const Texture &texture) {
	if (texture.type == TextureType::Gradient) {
		cairo_pattern_t *grad = cairo_pattern_create_linear(x, y, x, y + h);
		const Color &a = texture.color;
		const Color &b = texture.color_to;
		cairo_pattern_add_color_stop_rgba(grad, 0.0, a.r / 255.0, a.g / 255.0,
				a.b / 255.0, a.a / 255.0);
		cairo_pattern_add_color_stop_rgba(grad, 1.0, b.r / 255.0, b.g / 255.0,
				b.b / 255.0, b.a / 255.0);
		cairo_set_source(cr, grad);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		cairo_pattern_destroy(grad);
		return;
	}
	paint_rect(cr, x, y, w, h, texture.color);
}

void paint_fill(cairo_t *cr, double x, double y, double w, double h,
		const Paint &paint) {
	if (const auto *s = std::get_if<SolidPaint>(&paint)) {
		paint_rect(cr, x, y, w, h, s->color);
	} else if (const auto *g = std::get_if<GradientPaint>(&paint)) {
		paint_texture(cr, x, y, w, h,
				Texture{TextureType::Gradient, g->from, g->to});
	} else if (const auto *img = std::get_if<ImagePaint>(&paint)) {
		/* CPU renderer can't load images yet: use the safe fallback colour. */
		paint_rect(cr, x, y, w, h, img->fallback);
	} else if (const auto *sh = std::get_if<ShaderPaint>(&paint)) {
		/* Shaders need a GPU backend; draw the safe fallback colour. */
		paint_rect(cr, x, y, w, h, sh->fallback);
	}
}

void paint_text(cairo_t *cr, double x, double y, std::string_view text,
		const Color &color, const FontSpec &font) {
	PangoLayout *layout = make_layout(cr, text, font);
	set_source(cr, color);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

}  // namespace wb
