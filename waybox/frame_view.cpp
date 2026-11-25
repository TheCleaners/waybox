/*
 * Server-side decoration renderer: draws the window frame (titlebar/label/
 * buttons/borders) into a toplevel's scene container from a FrameStyle.
 */
#include "waybox/frame_view.hpp"

#include <algorithm>

#include <cairo.h>

#include "waybox/render.hpp"
#include "waybox/titlebar_draw.hpp"

namespace wb {

namespace {

void to_rgba(const Color &c, float out[4]) {
	out[0] = c.r / 255.0f;
	out[1] = c.g / 255.0f;
	out[2] = c.b / 255.0f;
	out[3] = c.a / 255.0f;
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
	TitlebarState st;
	st.title = title_;
	st.maximized = maximized_;
	st.shaded = shaded_;
	st.omnipresent = omnipresent_;
	st.icon = icon_;
	st.hovered = hovered_button_;
	st.pressed = pressed_button_;
	draw_titlebar(titlebar_->cr(), outer_w_, metrics_, style(), buttons_, left_, st);
	titlebar_->commit();
}

}  // namespace wb
