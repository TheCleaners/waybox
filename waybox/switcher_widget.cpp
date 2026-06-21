/*
 * Interactive Alt+Tab task switcher OSD: the wlroots/Cairo glue that draws the
 * window list and highlights the current selection. The cycle lifecycle (open,
 * advance, commit on modifier release, cancel) is driven from seat.cpp; this
 * class only renders and tracks the selected index.
 */
#include "waybox/switcher_widget.hpp"

#include <algorithm>

#include <cairo.h>

#include "waybox/render.hpp"
#include "waybox/server.h"

namespace wb {

SwitcherWidget::SwitcherWidget(struct wb_server *server,
		const SwitcherStyle &style, const SwitcherBehavior &behavior)
	: server_(server), style_(style), behavior_(behavior) {
	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_raise_to_top(&tree_->node);
	text_height_ = measure_text("Ag", style_.item_text.font).height;
}

SwitcherWidget::~SwitcherWidget() {
	canvas_.reset();
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
}

struct wlr_box SwitcherWidget::output_bounds() const {
	int cx = static_cast<int>(server_->cursor->cursor->x);
	int cy = static_cast<int>(server_->cursor->cursor->y);
	struct wlr_output *output =
			wlr_output_layout_output_at(server_->output_layout, cx, cy);
	if (output == nullptr)
		output = wlr_output_layout_output_at(server_->output_layout, 0, 0);
	struct wlr_box box = {};
	if (output != nullptr)
		wlr_output_layout_get_box(server_->output_layout, output, &box);
	if (box.width == 0 || box.height == 0)
		box = {0, 0, 1920, 1080};  /* sane fallback for headless */
	return box;
}

float SwitcherWidget::output_scale() const {
	int cx = static_cast<int>(server_->cursor->cursor->x);
	int cy = static_cast<int>(server_->cursor->cursor->y);
	struct wlr_output *output =
			wlr_output_layout_output_at(server_->output_layout, cx, cy);
	return (output != nullptr && output->scale > 0) ? output->scale : 1.0f;
}

void SwitcherWidget::open(std::vector<std::string> labels, int selected) {
	labels_ = std::move(labels);
	if (labels_.empty())
		return;
	selected_ = std::clamp(selected, 0, static_cast<int>(labels_.size()) - 1);

	const int pad_x = std::max(style_.panel.padding.left, 8);
	const int pad_y = std::max(style_.panel.padding.top, 6);
	const int row_pad_y = std::max(text_height_ / 3, 4);
	const int row_h = text_height_ + 2 * row_pad_y;

	int content_w = 0;
	for (const std::string &l : labels_)
		content_w = std::max(content_w, measure_text(l, style_.item_text.font).width);

	struct wlr_box b = output_bounds();
	int panel_w = content_w + 2 * pad_x;
	panel_w = std::min(panel_w, b.width - 40);
	panel_w = std::max(panel_w, 80);
	int panel_h = static_cast<int>(labels_.size()) * row_h + 2 * pad_y;
	panel_h = std::min(panel_h, b.height - 40);

	rect_.width = panel_w;
	rect_.height = panel_h;
	rect_.x = b.x + (b.width - panel_w) / 2;
	rect_.y = b.y + (b.height - panel_h) / 2;

	canvas_ = std::make_unique<SceneCanvas>(tree_, rect_.width, rect_.height,
			output_scale());
	canvas_->set_position(rect_.x, rect_.y);
	render();
}

void SwitcherWidget::select_next() {
	if (labels_.empty())
		return;
	int n = static_cast<int>(labels_.size());
	if (selected_ + 1 >= n) {
		if (!behavior_.wrap)
			return;
		selected_ = 0;
	} else {
		++selected_;
	}
	render();
}

void SwitcherWidget::select_prev() {
	if (labels_.empty())
		return;
	int n = static_cast<int>(labels_.size());
	if (selected_ <= 0) {
		if (!behavior_.wrap)
			return;
		selected_ = n - 1;
	} else {
		--selected_;
	}
	render();
}

void SwitcherWidget::render() {
	if (canvas_ == nullptr)
		return;
	cairo_t *cr = canvas_->cr();

	const Border &bd = style_.panel.border;
	if (bd.width > 0)
		paint_rect(cr, 0, 0, rect_.width, rect_.height, bd.color);
	paint_fill(cr, bd.width, bd.width, rect_.width - 2 * bd.width,
			rect_.height - 2 * bd.width, style_.panel.fill);

	const int pad_x = std::max(style_.panel.padding.left, 8);
	const int pad_y = std::max(style_.panel.padding.top, 6);
	const int row_pad_y = std::max(text_height_ / 3, 4);
	const int row_h = text_height_ + 2 * row_pad_y;

	for (std::size_t i = 0; i < labels_.size(); ++i) {
		int ry = pad_y + static_cast<int>(i) * row_h;
		bool active = static_cast<int>(i) == selected_;
		const StateStyle &st = style_.item.for_state(
				active ? WidgetState::Hover : WidgetState::Normal);
		if (active)
			paint_fill(cr, bd.width, ry, rect_.width - 2 * bd.width, row_h,
					st.fill);
		int text_y = ry + (row_h - text_height_) / 2;
		paint_text(cr, pad_x, text_y, labels_[i], st.fg, style_.item_text.font);
	}

	canvas_->commit();
}

}  // namespace wb
