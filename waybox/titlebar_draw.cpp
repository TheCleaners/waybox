/*
 * Drawing titlebars (see titlebar_draw.hpp).
 */
#include "waybox/titlebar_draw.hpp"

#include <algorithm>

#include "waybox/render.hpp"

namespace wb {

void paint_button_glyph(cairo_t *cr, const Rect &r, FrameButton kind,
		bool on, const Color &fg) {
	bool maximized = on;
	double cx = r.x + r.width / 2.0;
	double cy = r.y + r.height / 2.0;
	double s = r.width * 0.28;  /* glyph half-extent */
	cairo_set_source_rgba(cr, fg.r / 255.0, fg.g / 255.0, fg.b / 255.0,
			fg.a / 255.0);
	cairo_set_line_width(cr, 1.5);
	switch (kind) {
	case FrameButton::Iconify:
		cairo_move_to(cr, cx - s, cy + s);
		cairo_line_to(cr, cx + s, cy + s);
		cairo_stroke(cr);
		break;
	case FrameButton::Maximize:
		cairo_rectangle(cr, cx - s, cy - s, 2 * s, 2 * s);
		if (maximized) {  /* a second offset square hints "restore" */
			cairo_move_to(cr, cx - s + 3, cy - s - 3);
			cairo_line_to(cr, cx + s + 3, cy - s - 3);
			cairo_line_to(cr, cx + s + 3, cy + s - 3);
		}
		cairo_stroke(cr);
		break;
	case FrameButton::Close:
		cairo_move_to(cr, cx - s, cy - s);
		cairo_line_to(cr, cx + s, cy + s);
		cairo_move_to(cr, cx + s, cy - s);
		cairo_line_to(cr, cx - s, cy + s);
		cairo_stroke(cr);
		break;
	case FrameButton::Shade: {
		/* A rolled-up blind: a bar at the top, with an arrow up (roll up)
		 * or, rolled up, down (roll down). */
		cairo_move_to(cr, cx - s, cy - s);
		cairo_line_to(cr, cx + s, cy - s);
		double a = s * 0.6;
		double tip = on ? cy + s : cy - s * 0.2;
		double base = on ? cy + s * 0.2 : cy + s;
		cairo_move_to(cr, cx - a, base);
		cairo_line_to(cr, cx, tip);
		cairo_line_to(cr, cx + a, base);
		cairo_stroke(cr);
		break;
	}
	case FrameButton::AllDesktops: {
		/* Four desktops; filled when the window is on all of them. */
		double q = s * 0.8;
		for (int i = 0; i < 4; i++) {
			double x0 = (i % 2 == 0) ? cx - s : cx + s - q;
			double y0 = (i / 2 == 0) ? cy - s : cy + s - q;
			cairo_rectangle(cr, x0, y0, q, q);
		}
		if (on)
			cairo_fill(cr);
		else
			cairo_stroke(cr);
		break;
	}
	case FrameButton::Icon: {
		/* No icon: a small window (frame with a title strip). */
		double e = r.width * 0.32;
		cairo_rectangle(cr, cx - e, cy - e, 2 * e, 2 * e);
		cairo_move_to(cr, cx - e, cy - e + 3);
		cairo_line_to(cr, cx + e, cy - e + 3);
		cairo_stroke(cr);
		break;
	}
	}
}

/* Draw `icon` scaled to fit `r`, keeping its aspect ratio. */
void paint_icon(cairo_t *cr, const Rect &r, cairo_surface_t *icon) {
	int iw = cairo_image_surface_get_width(icon);
	int ih = cairo_image_surface_get_height(icon);
	if (iw <= 0 || ih <= 0)
		return;
	double scale = std::min(static_cast<double>(r.width) / iw,
			static_cast<double>(r.height) / ih);
	cairo_save(cr);
	cairo_translate(cr, r.x + (r.width - iw * scale) / 2.0,
			r.y + (r.height - ih * scale) / 2.0);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, icon, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}


FrameMetrics frame_metrics_for(const Theme &theme, const FrameStyle &style,
		int pad_y, int button_size, int resize_grab) {
	FrameMetrics m;
	m.border = theme.border_width > 0 ? theme.border_width : 1;
	int text_h = measure_text("Ag", style.label.font).height;
	/* Compact by default: one pixel tighter than the theme padding per side
	 * (Openbox's default frame feels a touch more compact). */
	int pad = pad_y >= 0 ? pad_y : std::max(theme.padding_y - 1, 1);
	m.titlebar = text_h + 2 * pad;
	if (m.titlebar < 16)
		m.titlebar = 16;
	/* Square button: explicit size wins; otherwise a fraction of the
	 * titlebar so the glyph stays inside it (and scales with the font). */
	m.button = button_size > 0 ? button_size : (m.titlebar * 2) / 3;
	if (m.button < 10)
		m.button = 10;
	m.title_pad = theme.padding_x > 0 ? theme.padding_x : 4;
	m.button_spacing = 2;
	m.corner = 10;
	/* Invisible margin that widens the border/corner resize grab area. */
	m.resize_grab = resize_grab >= 0 ? resize_grab : 8;
	return m;
}

void draw_titlebar(cairo_t *cr, int outer_w, const FrameMetrics &m, const FrameStyle &s,
		const std::vector<FrameButton> &buttons, int left, const TitlebarState &st) {
	Rect tb = titlebar_rect(outer_w, m);
	int w = tb.width;
	int h = tb.height;
	paint_fill(cr, 0, 0, w, h, s.title_bar.fill);

	/* The label fills the space between the left and right button groups.
	 * Button rects are frame-local; shift them into titlebar-local. */
	std::vector<Rect> brects = button_rects(outer_w, m, buttons, left);
	int label_left = m.title_pad;
	int label_right = w;
	if (left > 0 && left <= static_cast<int>(brects.size())) {
		const Rect &last = brects[left - 1];
		label_left = last.x + last.width - tb.x + m.title_pad;
	}
	if (left < static_cast<int>(brects.size()))
		label_right = brects[left].x - tb.x - m.title_pad;

	int label_x = label_left;
	TextExtents text = measure_text(st.title, s.label.font);
	/* Round to the nearest pixel (bias down) so the label isn't a pixel high
	 * when the leftover vertical space is odd. */
	int text_y = (h - text.height + 1) / 2;
	int room = label_right - label_left;
	if (s.label.justify == Justify::Center)
		label_x = label_left + (room - text.width) / 2;
	else if (s.label.justify == Justify::Right)
		label_x = label_right - text.width;
	if (label_x < label_left)
		label_x = label_left;
	/* A long title is cut with an ellipsis before the buttons. */
	if (room > 0)
		paint_text(cr, label_x, text_y, st.title, s.label.color, s.label.font, room);

	for (size_t i = 0; i < brects.size() && i < buttons.size(); ++i) {
		Rect r = brects[i];
		r.x -= tb.x;
		r.y -= tb.y;
		if (buttons[i] == FrameButton::Icon) {
			/* Not a push button: no hover or pressed look. */
			if (st.icon != nullptr)
				paint_icon(cr, r, st.icon);
			else
				paint_button_glyph(cr, r, buttons[i], st.maximized,
						s.button.for_state(WidgetState::Normal).fg);
			continue;
		}
		WidgetState ws = WidgetState::Normal;
		if (static_cast<int>(i) == st.pressed)
			ws = WidgetState::Pressed;
		else if (static_cast<int>(i) == st.hovered)
			ws = WidgetState::Hover;
		const StateStyle &bs = s.button.for_state(ws);
		/* The normal button background is the titlebar (parentrelative),
		 * already painted; only fill when hovered/pressed. */
		if (ws != WidgetState::Normal)
			paint_fill(cr, r.x, r.y, r.width, r.height, bs.fill);
		bool on = buttons[i] == FrameButton::Shade ? st.shaded
				: buttons[i] == FrameButton::AllDesktops ? st.omnipresent
				: st.maximized;
		paint_button_glyph(cr, r, buttons[i], on, bs.fg);
	}
}

void draw_frame_sample(cairo_t *cr, double x, double y, int client_w, int client_h,
		const FrameMetrics &m, const FrameStyle &s, const std::vector<FrameButton> &buttons,
		int left, const TitlebarState &st, const Color &client) {
	int outer_w = frame_width(client_w, m);
	int outer_h = frame_height(client_h, m);
	FrameInsets in = frame_insets(m);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	paint_rect(cr, 0, 0, outer_w, outer_h, s.border.color);
	Rect tb = titlebar_rect(outer_w, m);
	cairo_save(cr);
	cairo_translate(cr, tb.x, tb.y);
	cairo_rectangle(cr, 0, 0, tb.width, tb.height);
	cairo_clip(cr);
	draw_titlebar(cr, outer_w, m, s, buttons, left, st);
	cairo_restore(cr);
	if (client_h > 0)
		paint_rect(cr, in.left, in.top, client_w, client_h, client);
	cairo_restore(cr);
}

}  // namespace wb
