/*
 * Server-side decoration renderer: draws the window frame (titlebar/label/
 * buttons/borders) into a toplevel's scene container from a FrameStyle.
 */
#include "waybox/frame_view.hpp"

#include <algorithm>

#include <cairo.h>

#include "waybox/render.hpp"

namespace wb {

namespace {

void to_rgba(const Color &c, float out[4]) {
	out[0] = c.r / 255.0f;
	out[1] = c.g / 255.0f;
	out[2] = c.b / 255.0f;
	out[3] = c.a / 255.0f;
}

/* Draw a button glyph (iconify/maximize/close) centred in `r`. */
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

}  // namespace

FrameView::FrameView(struct wlr_scene_tree *container, const FrameStyle &active,
		const FrameStyle &inactive, const FrameMetrics &metrics,
		std::vector<FrameButton> buttons, int left)
	: active_(active), inactive_(inactive), metrics_(metrics),
	  buttons_(std::move(buttons)), left_(left) {
	/* A fully transparent rect spanning the whole frame (including the invisible
	 * resize-grab margin) so the margin is hit-testable for edge/corner resize
	 * even though it paints nothing. It sits at the very bottom. */
	float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	grab_ = wlr_scene_rect_create(container, 1, 1, clear);

	float bg[4];
	to_rgba(style().border.color, bg);
	bg_ = wlr_scene_rect_create(container, 1, 1, bg);

	/* Stack from the bottom: grab rect, then the border backdrop just above it;
	 * the titlebar canvas and the caller's client surface stay on top. */
	wlr_scene_node_lower_to_bottom(&grab_->node);
	wlr_scene_node_place_above(&bg_->node, &grab_->node);
}

FrameView::~FrameView() {
	titlebar_.reset();
	if (bg_ != nullptr)
		wlr_scene_node_destroy(&bg_->node);
	if (grab_ != nullptr)
		wlr_scene_node_destroy(&grab_->node);
}

std::vector<Rect> FrameView::button_rects() const {
	return wb::button_rects(outer_w_, metrics_, buttons_, left_);
}

void FrameView::set_client_size(int client_w, int client_h) {
	outer_w_ = frame_width(client_w, metrics_);
	outer_h_ = frame_height(client_h, metrics_);
	layout();
}

void FrameView::set_active(bool active) {
	if (active_state_ == active)
		return;
	active_state_ = active;
	float bg[4];
	to_rgba(style().border.color, bg);
	if (bg_ != nullptr)
		wlr_scene_rect_set_color(bg_, bg);
	render_titlebar();
}

void FrameView::set_title(std::string title) {
	if (title_ == title)
		return;
	title_ = std::move(title);
	render_titlebar();
}

void FrameView::set_maximized(bool maximized) {
	if (maximized_ == maximized)
		return;
	maximized_ = maximized;
	render_titlebar();
}

void FrameView::set_shaded(bool shaded) {
	if (shaded_ == shaded)
		return;
	shaded_ = shaded;
	render_titlebar();
}

void FrameView::set_omnipresent(bool omnipresent) {
	if (omnipresent_ == omnipresent)
		return;
	omnipresent_ = omnipresent;
	render_titlebar();
}

void FrameView::set_icon(cairo_surface_t *icon) {
	if (icon_ == icon)
		return;
	icon_ = icon;
	render_titlebar();
}

void FrameView::set_hovered_button(int index) {
	if (hovered_button_ == index)
		return;
	hovered_button_ = index;
	render_titlebar();
}

void FrameView::set_pressed_button(int index) {
	if (pressed_button_ == index)
		return;
	pressed_button_ = index;
	render_titlebar();
}

void FrameView::layout() {
	int g = metrics_.resize_grab;
	if (grab_ != nullptr) {
		/* The invisible grab rect extends `g` past the visible frame on every
		 * side; its negative offset means hit-tests in the margin land here
		 * (and resolve to border/corner resize) without enlarging the frame's
		 * layout insets. */
		wlr_scene_rect_set_size(grab_, outer_w_ + 2 * g, outer_h_ + 2 * g);
		wlr_scene_node_set_position(&grab_->node, -g, -g);
	}
	if (bg_ != nullptr) {
		wlr_scene_rect_set_size(bg_, outer_w_, outer_h_);
		wlr_scene_node_set_position(&bg_->node, 0, 0);
	}

	if (metrics_.titlebar <= 0) {
		titlebar_.reset();
		return;
	}
	Rect tb = titlebar_rect(outer_w_, metrics_);
	/* Keep the canvas while its size is unchanged (layout runs on each of
	 * the window's commits); the setters redraw it when its content does. */
	if (titlebar_ && titlebar_->width() == tb.width && titlebar_->height() == tb.height) {
		titlebar_->set_position(tb.x, tb.y);
		return;
	}
	titlebar_ = std::make_unique<SceneCanvas>(
			bg_ != nullptr ? bg_->node.parent : nullptr, tb.width, tb.height,
			scale_);
	titlebar_->set_position(tb.x, tb.y);
	render_titlebar();
}

void FrameView::render_titlebar() {
	if (!titlebar_)
		return;
	const FrameStyle &s = style();
	cairo_t *cr = titlebar_->cr();
	int w = titlebar_->width();
	int h = titlebar_->height();

	paint_fill(cr, 0, 0, w, h, s.title_bar.fill);

	/* The label fills the space between the left and right button groups.
	 * Button rects are frame-local; shift them into titlebar-local (the
	 * titlebar canvas starts at the titlebar origin). */
	std::vector<Rect> brects = button_rects();
	int tb_x = titlebar_rect(outer_w_, metrics_).x;  /* = border */
	int label_left = metrics_.title_pad;
	int label_right = w;
	if (left_ > 0 && left_ <= static_cast<int>(brects.size())) {
		const Rect &last = brects[left_ - 1];
		label_left = last.x + last.width - tb_x + metrics_.title_pad;
	}
	if (left_ < static_cast<int>(brects.size()))
		label_right = brects[left_].x - tb_x - metrics_.title_pad;

	int label_x = label_left;
	TextExtents text = measure_text(title_, s.label.font);
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
		paint_text(cr, label_x, text_y, title_, s.label.color, s.label.font, room);

	for (size_t i = 0; i < brects.size() && i < buttons_.size(); ++i) {
		/* button rects are frame-local; convert to titlebar-canvas-local. */
		Rect r = brects[i];
		r.x -= tb_x;
		r.y -= titlebar_rect(outer_w_, metrics_).y;
		if (buttons_[i] == FrameButton::Icon) {
			/* Not a push button: no hover or pressed look. */
			if (icon_ != nullptr)
				paint_icon(cr, r, icon_);
			else
				paint_button_glyph(cr, r, buttons_[i], maximized_,
						s.button.for_state(WidgetState::Normal).fg);
			continue;
		}
		WidgetState ws = WidgetState::Normal;
		if (static_cast<int>(i) == pressed_button_)
			ws = WidgetState::Pressed;
		else if (static_cast<int>(i) == hovered_button_)
			ws = WidgetState::Hover;
		const StateStyle &bs = s.button.for_state(ws);
		/* The normal button background is the titlebar (parentrelative), already
		 * painted; only fill when hovered/pressed so the accent shows. */
		if (ws != WidgetState::Normal)
			paint_fill(cr, r.x, r.y, r.width, r.height, bs.fill);
		bool on = buttons_[i] == FrameButton::Shade ? shaded_
				: buttons_[i] == FrameButton::AllDesktops ? omnipresent_
				: maximized_;
		paint_button_glyph(cr, r, buttons_[i], on, bs.fg);
	}

	titlebar_->commit();
}

}  // namespace wb
