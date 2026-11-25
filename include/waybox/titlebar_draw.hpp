#ifndef WB_TITLEBAR_DRAW_HPP
#define WB_TITLEBAR_DRAW_HPP

/*
 * Drawing a window's titlebar with Cairo: the same code for Waybox's frames
 * and wbconf's previews, so the preview is what windows look like. No
 * wlroots; the caller provides the Cairo context.
 */
#include <cairo.h>
#include <string>
#include <vector>

#include "waybox/frame.hpp"
#include "waybox/style.hpp"

namespace wb {

struct TitlebarState {
	std::string title;
	bool maximized = false;
	bool shaded = false;
	bool omnipresent = false;
	cairo_surface_t *icon = nullptr;  /* the window icon (N), or a glyph */
	int hovered = -1, pressed = -1;   /* button indices */
};

/* A frame's metrics from the theme and the titlebar settings: pad_y < 0
 * and button_size <= 0 mean automatic, resize_grab < 0 the default. */
FrameMetrics frame_metrics_for(const Theme &theme, const FrameStyle &style,
		int pad_y, int button_size, int resize_grab);

/* The titlebar of a frame `outer_w` wide, drawn at (0, 0): fill, title,
 * buttons in `buttons` order (the first `left` on the left). */
void draw_titlebar(cairo_t *cr, int outer_w, const FrameMetrics &m, const FrameStyle &s,
		const std::vector<FrameButton> &buttons, int left, const TitlebarState &st);

/* A whole sample window at (x, y): border, titlebar and a client area of
 * client_w x client_h (filled with `client`). For previews. */
void draw_frame_sample(cairo_t *cr, double x, double y, int client_w, int client_h,
		const FrameMetrics &m, const FrameStyle &s, const std::vector<FrameButton> &buttons,
		int left, const TitlebarState &st, const Color &client);

/* A button glyph centred in `r`; `on` is its state (maximized, rolled up,
 * on all desktops). */
void paint_button_glyph(cairo_t *cr, const Rect &r, FrameButton kind, bool on,
		const Color &fg);
/* `icon` scaled to fit `r`, keeping its aspect ratio. */
void paint_icon(cairo_t *cr, const Rect &r, cairo_surface_t *icon);

}  // namespace wb

#endif
