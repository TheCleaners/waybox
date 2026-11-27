/*
 * Interactive Alt+Tab task switcher OSD: the wlroots/Cairo glue that draws the
 * window list and highlights the current selection. The cycle lifecycle (open,
 * advance, commit on modifier release, cancel) is driven from seat.cpp; this
 * class only renders and tracks the selected index.
 */
#include "waybox/switcher_widget.hpp"
#include "waybox/cycle_outline.hpp"

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

void SwitcherWidget::remove_entry(int i) {
	if (i < 0 || i >= static_cast<int>(entries_.size()))
		return;
	std::vector<Entry> rest = std::move(entries_);
	entries_.clear();
	if (rest[i].icon != nullptr)
		cairo_surface_destroy(rest[i].icon);
	rest.erase(rest.begin() + i);
	int sel = selected_ > i ? selected_ - 1 : selected_;
	if (rest.empty()) {
		canvas_.reset();
		return;
	}
	open(rest, std::min(sel, static_cast<int>(rest.size()) - 1));
	/* open() took its own reference to each icon: the ones `rest` held go. */
	for (Entry &e : rest)
		if (e.icon != nullptr)
			cairo_surface_destroy(e.icon);
}

void SwitcherWidget::raise() {
	wlr_scene_node_raise_to_top(&tree_->node);
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
		content_w = std::max(content_w, icon_w + measure_text(e.label, style_.item_text.font).width +
				(e.note.empty() ? 0 : measure_text("  " + e.note, style_.item_text.font).width));

	struct wlr_box b = output_bounds();
	/* Room for each row's close button (x) after its title. */
	int panel_w = content_w + 2 * pad_x +
			(behavior_.close ? std::max(text_height_, 12) + pad_x : 0);
	panel_w = std::min(panel_w, b.width - 40);
	panel_w = std::max(panel_w, 80);
	int panel_h = static_cast<int>(entries_.size()) * row_h + 2 * pad_y;
	panel_h = std::min(panel_h, b.height - 40);

	rect_.width = panel_w;
	rect_.height = panel_h;
	rect_.x = b.x + (b.width - panel_w) / 2;
	rect_.y = osd_y(style_.position, b.y, b.height, panel_h);

	canvas_ = std::make_unique<SceneCanvas>(tree_, rect_.width, rect_.height,
			output_scale());
	canvas_->set_position(rect_.x, rect_.y);
	render();
	wlr_log(WLR_INFO, "wb-switcher-panel %d,%d %dx%d rows %zu row_h %d pad %d", rect_.x, rect_.y,
			rect_.width, rect_.height, entries_.size(), this->row_h(), this->pad_y());
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

int SwitcherWidget::row_h() const {
	const int row_pad_y = std::max(text_height_ / 3, 4);
	return std::max(text_height_, icon_size()) + 2 * row_pad_y;
}

int SwitcherWidget::pad_y() const {
	return std::max(style_.panel.padding.top, 6);
}

/* A row's close button: a square as tall as the text, at its right end. */
Rect SwitcherWidget::close_rect(int row) const {
	const int pad_x = std::max(style_.panel.padding.left, 8);
	const int size = std::max(text_height_, 12);
	int ry = pad_y() + row * row_h();
	return Rect{rect_.width - pad_x - size, ry + (row_h() - size) / 2, size, size};
}

SwitcherWidget::Hit SwitcherWidget::hit(double lx, double ly) const {
	Hit h;
	double x = lx - rect_.x, y = ly - rect_.y;
	if (canvas_ == nullptr || x < 0 || y < 0 || x >= rect_.width || y >= rect_.height)
		return h;
	int row = static_cast<int>((y - pad_y()) / row_h());
	if (y < pad_y() || row < 0 || row >= static_cast<int>(entries_.size()))
		return h;
	h.row = row;
	if (!behavior_.close)
		return h;  /* no close buttons */
	Rect c = close_rect(row);
	/* A little margin around the x: easier to hit. */
	h.close = x >= c.x - 4 && x < c.x + c.width + 4 && y >= c.y - 4 && y < c.y + c.height + 4;
	return h;
}

bool SwitcherWidget::on_motion(double lx, double ly) {
	Hit h = hit(lx, ly);
	if (h.row != hover_ || h.close != hover_close_) {
		hover_ = h.row;
		hover_close_ = h.close;
		render();
	}
	double x = lx - rect_.x, y = ly - rect_.y;
	return canvas_ != nullptr && x >= 0 && y >= 0 && x < rect_.width && y < rect_.height;
}

void SwitcherWidget::set_note(int i, const std::string &note) {
	if (i < 0 || i >= static_cast<int>(entries_.size()) || entries_[i].note == note)
		return;
	/* Laid out again: the row may need to be wider. */
	std::vector<Entry> rest = std::move(entries_);
	entries_.clear();
	rest[i].note = note;
	open(rest, selected_);
	for (Entry &e : rest)
		if (e.icon != nullptr)
			cairo_surface_destroy(e.icon);
}

void SwitcherWidget::select(int i) {
	if (i < 0 || i >= static_cast<int>(entries_.size()) || i == selected_)
		return;
	selected_ = i;
	render();
}

void SwitcherWidget::render() {
	if (canvas_ == nullptr)
		return;
	cairo_t *cr = canvas_->cr();

	paint_panel(cr, rect_.width, rect_.height, style_.panel);
	const Border &bd = style_.panel.border;

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
		/* The title stops short of the close button. */
		Rect cr_box = close_rect(static_cast<int>(i));
		cairo_save(cr);
		cairo_rectangle(cr, 0, ry, behavior_.close ? cr_box.x - pad_x / 2 : rect_.width, row_h);
		cairo_clip(cr);
		paint_text(cr, pad_x + icon_w, text_y, entries_[i].label, st.fg,
				style_.item_text.font);
		if (!entries_[i].note.empty()) {
			Color dim = st.fg;
			dim.a = static_cast<uint8_t>(dim.a * 0.65);
			int lw = measure_text(entries_[i].label, style_.item_text.font).width;
			paint_text(cr, pad_x + icon_w + lw, text_y, "  " + entries_[i].note, dim,
					style_.item_text.font);
		}
		cairo_restore(cr);
		/* Its close button: on the selected row and the one under the
		 * pointer. */
		bool hovered = static_cast<int>(i) == hover_;
		if (behavior_.close && (active || hovered)) {
			const Color &fg = st.fg;
			double cx = cr_box.x + cr_box.width / 2.0, cy = cr_box.y + cr_box.height / 2.0;
			double r = cr_box.width / 2.0;
			/* Always in a box (the frame's buttons are boxes too), slightly
			 * rounded, in the row's text colour: faint at rest; filled and
			 * brighter while the pointer is on it. */
			bool on_x = hovered && hover_close_;
			double bx = cr_box.x + 0.5, by = cr_box.y + 0.5;
			double bw = cr_box.width - 1.0, bh = cr_box.height - 1.0;
			double rad = std::max(2.0, bw / 5.0);
			const double pi = 3.14159265358979;
			cairo_new_sub_path(cr);
			cairo_arc(cr, bx + bw - rad, by + rad, rad, -pi / 2, 0);
			cairo_arc(cr, bx + bw - rad, by + bh - rad, rad, 0, pi / 2);
			cairo_arc(cr, bx + rad, by + bh - rad, rad, pi / 2, pi);
			cairo_arc(cr, bx + rad, by + rad, rad, pi, 3 * pi / 2);
			cairo_close_path(cr);
			if (on_x) {
				cairo_set_source_rgba(cr, fg.r / 255.0, fg.g / 255.0, fg.b / 255.0, 0.25);
				cairo_fill_preserve(cr);
			}
			cairo_set_source_rgba(cr, fg.r / 255.0, fg.g / 255.0, fg.b / 255.0,
					on_x ? 0.9 : 0.35);
			cairo_set_line_width(cr, 1.0);
			cairo_stroke(cr);
			double a = r * 0.45;
			cairo_set_source_rgba(cr, fg.r / 255.0, fg.g / 255.0, fg.b / 255.0,
					hovered && hover_close_ ? 1.0 : 0.75);
			cairo_set_line_width(cr, std::max(1.5, r / 5.0));
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_move_to(cr, cx - a, cy - a);
			cairo_line_to(cr, cx + a, cy + a);
			cairo_move_to(cr, cx + a, cy - a);
			cairo_line_to(cr, cx - a, cy + a);
			cairo_stroke(cr);
		}
	}

	canvas_->commit();
	wlr_scene_buffer_set_opacity(canvas_->node(),
			static_cast<float>(std::clamp(style_.panel.opacity, 0.1, 1.0)));
}

}  // namespace wb
