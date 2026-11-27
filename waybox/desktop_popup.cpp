#include "waybox/desktop_popup.hpp"

#include <algorithm>

#include <cairo.h>

#include "waybox/render.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"

namespace wb {

DesktopPopup::DesktopPopup(struct wb_server *server, const SwitcherStyle &style)
	: server_(server), style_(style) {
	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	timer_ = wl_event_loop_add_timer(server_->wl_event_loop, [](void *data) {
		static_cast<DesktopPopup *>(data)->hide();
		return 0;
	}, this);
}

DesktopPopup::~DesktopPopup() {
	if (timer_ != nullptr)
		wl_event_source_remove(timer_);
	canvases_.clear();
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
}

void DesktopPopup::hide() {
	if (canvases_.empty())
		return;
	canvases_.clear();
	wlr_log(WLR_INFO, "wb-desktop-popup hidden");
}

void DesktopPopup::show(const std::string &label, int duration_ms) {
	/* The monitor under the pointer. */
	int cx = static_cast<int>(server_->cursor->cursor->x);
	int cy = static_cast<int>(server_->cursor->cursor->y);
	struct wlr_output *output =
			wlr_output_layout_output_at(server_->output_layout, cx, cy);
	if (output == nullptr)
		output = wlr_output_layout_get_center_output(server_->output_layout);
	show_on(output, label, duration_ms);
}

void DesktopPopup::show_on(struct wlr_output *output, const std::string &label,
		int duration_ms) {
	show_on_each({output}, label, duration_ms);
}

void DesktopPopup::show_on_each(const std::vector<struct wlr_output *> &outputs,
		const std::string &label, int duration_ms) {
	if (duration_ms <= 0 || session_locked(server_))
		return;
	canvases_.clear();
	int shown = 0;
	for (struct wlr_output *output : outputs)
		if (output != nullptr && add_on(output, label))
			++shown;
	if (shown == 0)
		return;
	/* Above windows and panels; a lock screen stays above everything. */
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);
	wl_event_source_timer_update(timer_, duration_ms);
	wlr_log(WLR_INFO, "wb-desktop-popup %s on %d", label.c_str(), shown);
}

/* The label in the middle (or as the OSD's position says) of `output`. */
bool DesktopPopup::add_on(struct wlr_output *output, const std::string &label) {
	struct wlr_box box = {};
	wlr_output_layout_get_box(server_->output_layout, output, &box);
	if (box.width <= 0 || box.height <= 0)
		return false;

	const TextExtents text = measure_text(label, style_.item_text.font);
	const int pad_x = std::max(style_.panel.padding.left, 8) * 2;
	const int pad_y = std::max(style_.panel.padding.top, 6) * 2;
	int w = std::min(text.width + 2 * pad_x, std::max(box.width - 40, 80));
	int h = text.height + 2 * pad_y;
	int x = box.x + (box.width - w) / 2;
	int y = osd_y(style_.position, box.y, box.height, h);

	auto canvas = std::make_unique<SceneCanvas>(tree_, w, h,
			output->scale > 0 ? output->scale : 1.0f);
	canvas->set_position(x, y);
	cairo_t *cr = canvas->cr();
	paint_panel(cr, w, h, style_.panel);
	const StateStyle &st = style_.item.for_state(WidgetState::Normal);
	paint_text(cr, (w - text.width) / 2, (h - text.height) / 2, label, st.fg,
			style_.item_text.font);
	canvas->commit();
	wlr_scene_buffer_set_opacity(canvas->node(),
			static_cast<float>(std::clamp(style_.panel.opacity, 0.1, 1.0)));
	canvases_.push_back(std::move(canvas));
	return true;
}

}  // namespace wb
