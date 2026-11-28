#include "waybox/confirm_card.hpp"

#include <algorithm>
#include <utility>

#include <xkbcommon/xkbcommon.h>

#include "waybox/render.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"

namespace wb {

namespace {

constexpr int kPad = 22;      /* around everything */
constexpr int kButtonGap = 10;

}  // namespace

ConfirmCard::ConfirmCard(struct wb_server *server, const SwitcherStyle &style, std::string title,
		std::string message, std::string yes, std::string no)
	: server_(server), style_(style), title_(std::move(title)), message_(std::move(message)),
	  yes_(std::move(yes)), no_(std::move(no)) {
	/* The monitor under the pointer. */
	int cx = static_cast<int>(server_->cursor->cursor->x);
	int cy = static_cast<int>(server_->cursor->cursor->y);
	struct wlr_output *output = wlr_output_layout_output_at(server_->output_layout, cx, cy);
	struct wlr_box b = {0, 0, 1920, 1080};
	if (output != nullptr)
		wlr_output_layout_get_box(server_->output_layout, output, &b);

	const FontSpec &font = style_.item_text.font;
	FontSpec bold = font;
	bold.size_pt = font.size_pt * 13 / 10;
	bold.weight = 700;
	int line_h = measure_text("Ag", font).height;
	/* Both buttons as wide as the wider label. */
	int bw = std::max(measure_text(yes_, font).width, measure_text(no_, font).width) + 36;
	int bh = line_h + 14;
	int width = std::max({measure_text(title_, bold).width, measure_text(message_, font).width,
			2 * bw + kButtonGap, 260});
	int height = measure_text(title_, bold).height + 8 + line_h + 20 + bh;
	rect_.width = width + 2 * kPad;
	rect_.height = height + 2 * kPad;
	rect_.x = b.x + (b.width - rect_.width) / 2;
	rect_.y = b.y + (b.height - rect_.height) / 2;
	/* The buttons at the bottom right: No (Cancel), then Yes. */
	int by = rect_.height - kPad - bh;
	buttons_[1] = {rect_.width - kPad - bw, by, bw, bh};
	buttons_[0] = {buttons_[1].x - kButtonGap - bw, by, bw, bh};

	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);
	float scale = output != nullptr && output->scale > 0 ? output->scale : 1.0f;
	canvas_ = std::make_unique<SceneCanvas>(tree_, rect_.width, rect_.height, scale);
	canvas_->set_position(rect_.x, rect_.y);
	render();
	wlr_log(WLR_INFO, "wb-confirm open %s", title_.c_str());
}

ConfirmCard::~ConfirmCard() {
	wlr_log(WLR_INFO, "%s", "wb-confirm close");
	canvas_.reset();
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
}

int ConfirmCard::button_at(double x, double y) const {
	double lx = x - rect_.x, ly = y - rect_.y;
	for (int i = 0; i < 2; ++i) {
		const Rect &r = buttons_[i];
		if (lx >= r.x && lx < r.x + r.width && ly >= r.y && ly < r.y + r.height)
			return i;
	}
	return -1;
}

ConfirmCard::Answer ConfirmCard::on_key(uint32_t sym) {
	switch (sym) {
	case XKB_KEY_Escape:
		return Answer::No;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		return chosen_ == 1 ? Answer::Yes : Answer::No;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		chosen_ = 1 - chosen_;
		render();
		return Answer::None;
	default:
		return Answer::None;
	}
}

void ConfirmCard::on_motion(double x, double y) {
	int h = button_at(x, y);
	if (h != hover_) {
		hover_ = h;
		render();
	}
}

ConfirmCard::Answer ConfirmCard::on_press(double x, double y) {
	int b = button_at(x, y);
	if (b >= 0)
		return b == 1 ? Answer::Yes : Answer::No;
	double lx = x - rect_.x, ly = y - rect_.y;
	bool inside = lx >= 0 && ly >= 0 && lx < rect_.width && ly < rect_.height;
	return inside ? Answer::None : Answer::No;  /* outside: as Cancel */
}

void ConfirmCard::render() {
	if (canvas_ == nullptr)
		return;
	cairo_t *cr = canvas_->cr();
	paint_panel(cr, rect_.width, rect_.height, style_.panel);
	const FontSpec &font = style_.item_text.font;
	FontSpec bold = font;
	bold.size_pt = font.size_pt * 13 / 10;
	bold.weight = 700;
	const StateStyle &normal = style_.item.for_state(WidgetState::Normal);
	const StateStyle &hilite = style_.item.for_state(WidgetState::Hover);

	int y = kPad;
	paint_text(cr, kPad, y, title_, normal.fg, bold);
	y += measure_text(title_, bold).height + 8;
	Color dim = normal.fg;
	dim.a = static_cast<uint8_t>(dim.a * 0.75);
	paint_text(cr, kPad, y, message_, dim, font);

	/* The chosen button in the theme's highlight; the other outlined (and
	 * filled faintly under the pointer). */
	const std::string *labels[2] = {&no_, &yes_};
	for (int i = 0; i < 2; ++i) {
		const Rect &r = buttons_[i];
		bool chosen = i == chosen_;
		const Color &fg = chosen ? hilite.fg : normal.fg;
		if (chosen) {
			paint_fill(cr, r.x, r.y, r.width, r.height, hilite.fill);
		} else if (i == hover_) {
			Color f = normal.fg;
			f.a = 40;
			paint_rect(cr, r.x, r.y, r.width, r.height, f);
		}
		Color edge = fg;
		edge.a = chosen ? 0 : 110;
		if (edge.a != 0) {
			cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.width - 1.0, r.height - 1.0);
			cairo_set_source_rgba(cr, edge.r / 255.0, edge.g / 255.0, edge.b / 255.0,
					edge.a / 255.0);
			cairo_set_line_width(cr, 1.0);
			cairo_stroke(cr);
		}
		TextExtents t = measure_text(*labels[i], font);
		paint_text(cr, r.x + (r.width - t.width) / 2.0, r.y + (r.height - t.height) / 2.0,
				*labels[i], fg, font);
	}
	canvas_->commit();
	wlr_scene_buffer_set_opacity(canvas_->node(),
			static_cast<float>(std::clamp(style_.panel.opacity, 0.1, 1.0)));
}

}  // namespace wb
