/*
 * Server-side decoration renderer: draws the window frame (titlebar/label/
 * buttons/borders) into a toplevel's scene container from a FrameStyle.
 */
#include "waybox/frame_view.hpp"

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
		bool maximized, const Color &fg) {
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
	}
}

}  // namespace

FrameView::FrameView(struct wlr_scene_tree *container, const FrameStyle &active,
		const FrameStyle &inactive, const FrameMetrics &metrics,
		std::vector<FrameButton> buttons)
	: active_(active), inactive_(inactive), metrics_(metrics),
	  buttons_(std::move(buttons)) {
	float bg[4];
	to_rgba(style().border.color, bg);
	bg_ = wlr_scene_rect_create(container, 1, 1, bg);
	/* Keep the decoration behind the client surface (the caller places the
	 * surface above us in the same container). */
	wlr_scene_node_lower_to_bottom(&bg_->node);
}

FrameView::~FrameView() {
	titlebar_.reset();
	if (bg_ != nullptr)
		wlr_scene_node_destroy(&bg_->node);
}

std::vector<Rect> FrameView::button_rects() const {
	return wb::button_rects(outer_w_, metrics_, buttons_);
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

void FrameView::layout() {
	if (bg_ != nullptr) {
		wlr_scene_rect_set_size(bg_, outer_w_, outer_h_);
		wlr_scene_node_set_position(&bg_->node, 0, 0);
	}

	if (metrics_.titlebar <= 0) {
		titlebar_.reset();
		return;
	}
	Rect tb = titlebar_rect(outer_w_, metrics_);
	/* (Re)create the titlebar canvas when its size changes. */
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

	/* Buttons occupy the right; the label fills the space left of them. */
	std::vector<Rect> brects = button_rects();
	int label_right = w;
	if (!brects.empty()) {
		/* button rects are in frame-local coords; shift into titlebar-local
		 * (the titlebar starts at (border, border)). */
		label_right = brects.front().x - metrics_.border - metrics_.title_pad;
	}

	int label_x = metrics_.title_pad;
	int text_h = measure_text(title_, s.label.font).height;
	int text_y = (h - text_h) / 2;
	if (s.label.justify == Justify::Center) {
		int tw = measure_text(title_, s.label.font).width;
		label_x = (label_right - tw) / 2;
		if (label_x < metrics_.title_pad)
			label_x = metrics_.title_pad;
	}
	paint_text(cr, label_x, text_y, title_, s.label.color, s.label.font);

	for (size_t i = 0; i < brects.size() && i < buttons_.size(); ++i) {
		/* button rects are frame-local; convert to titlebar-local. */
		Rect r = brects[i];
		r.x -= metrics_.border;
		r.y -= metrics_.border;
		const StateStyle &bs = s.button.for_state(WidgetState::Normal);
		paint_fill(cr, r.x, r.y, r.width, r.height, bs.fill);
		paint_button_glyph(cr, r, buttons_[i], maximized_, bs.fg);
	}

	titlebar_->commit();
}

}  // namespace wb
