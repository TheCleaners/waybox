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
#include "waybox/session_lock.h"

namespace wb {

SwitcherWidget::SwitcherWidget(struct wb_server *server,
		const SwitcherStyle &style, const SwitcherBehavior &behavior)
	: server_(server), style_(style), behavior_(behavior) {
	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);
	text_height_ = measure_text("Ag", style_.item_text.font).height;
}

SwitcherWidget::~SwitcherWidget() {
	for (Entry &e : entries_)
		if (e.icon != nullptr)
			cairo_surface_destroy(e.icon);
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

/* The icon edge: the text height, when icons are shown. */
int SwitcherWidget::icon_size() const {
	return behavior_.show_icons ? text_height_ + 4 : 0;
}

void SwitcherWidget::open(std::vector<Entry> entries, int selected) {
	entries_ = std::move(entries);
	for (Entry &e : entries_)
		if (e.icon != nullptr)
			cairo_surface_reference(e.icon);
	if (entries_.empty())
		return;
	selected_ = std::clamp(selected, 0, static_cast<int>(entries_.size()) - 1);

	const int pad_x = std::max(style_.panel.padding.left, 8);
	const int pad_y = std::max(style_.panel.padding.top, 6);
	const int row_pad_y = std::max(text_height_ / 3, 4);
	const int row_h = std::max(text_height_, icon_size()) + 2 * row_pad_y;
	const int icon_w = icon_size() > 0 ? icon_size() + pad_x / 2 + 2 : 0;

	int content_w = 0;
	for (const Entry &e : entries_)
		content_w = std::max(content_w,
				icon_w + measure_text(e.label, style_.item_text.font).width);

	struct wlr_box b = output_bounds();
	int panel_w = content_w + 2 * pad_x;
	panel_w = std::min(panel_w, b.width - 40);
	panel_w = std::max(panel_w, 80);
	int panel_h = static_cast<int>(entries_.size()) * row_h + 2 * pad_y;
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
	if (entries_.empty())
		return;
	int n = static_cast<int>(entries_.size());
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
	if (entries_.empty())
		return;
	int n = static_cast<int>(entries_.size());
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
	const int row_h = std::max(text_height_, icon_size()) + 2 * row_pad_y;
	const int isz = icon_size();
	const int icon_w = isz > 0 ? isz + pad_x / 2 + 2 : 0;

	for (std::size_t i = 0; i < entries_.size(); ++i) {
		int ry = pad_y + static_cast<int>(i) * row_h;
		bool active = static_cast<int>(i) == selected_;
		const StateStyle &st = style_.item.for_state(
				active ? WidgetState::Hover : WidgetState::Normal);
		if (active)
			paint_fill(cr, bd.width, ry, rect_.width - 2 * bd.width, row_h,
					st.fill);
		if (isz > 0 && entries_[i].icon != nullptr) {
			cairo_surface_t *icon = entries_[i].icon;
			int iw = cairo_image_surface_get_width(icon);
			int ih = cairo_image_surface_get_height(icon);
			if (iw > 0 && ih > 0) {
				double scale = std::min(static_cast<double>(isz) / iw,
						static_cast<double>(isz) / ih);
				cairo_save(cr);
				cairo_translate(cr, pad_x + (isz - iw * scale) / 2.0,
						ry + (row_h - ih * scale) / 2.0);
				cairo_scale(cr, scale, scale);
				cairo_set_source_surface(cr, icon, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
				cairo_paint(cr);
				cairo_restore(cr);
			}
		}
		int text_y = ry + (row_h - text_height_) / 2;
		paint_text(cr, pad_x + icon_w, text_y, entries_[i].label, st.fg,
				style_.item_text.font);
	}

	canvas_->commit();
}

}  // namespace wb
