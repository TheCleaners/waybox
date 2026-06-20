#ifndef WB_RENDER_HPP
#define WB_RENDER_HPP

#include <string_view>

#include <cairo.h>

#include "waybox/theme.hpp"

namespace wb {

/*
 * Cairo/Pango painting primitives for compositor-drawn chrome. These operate on
 * a caller-supplied cairo_t (already scaled to the output) in logical pixels;
 * they have no wlroots dependency, so they can be pixel-tested in isolation
 * (test/render_test.cpp). The scene/buffer plumbing lives in scene_buffer.cpp.
 */

struct FontSpec {
	const char *family = "sans";
	int size_pt = 10;
	bool bold = false;
};

struct TextExtents {
	int width = 0;
	int height = 0;
};

/* Measure one line of text in logical pixels (uses a throwaway context). */
TextExtents measure_text(std::string_view text, const FontSpec &font);

/* Fill a rectangle with a solid colour (alpha respected). */
void paint_rect(cairo_t *cr, double x, double y, double w, double h,
		const Color &color);

/* Fill a rectangle with a texture: solid, or a vertical gradient from
 * color -> color_to. */
void paint_texture(cairo_t *cr, double x, double y, double w, double h,
		const Texture &texture);

/* Draw one line of text with its top-left at (x, y). */
void paint_text(cairo_t *cr, double x, double y, std::string_view text,
		const Color &color, const FontSpec &font);

}  // namespace wb

#endif /* WB_RENDER_HPP */
