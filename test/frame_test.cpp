#include "wb_test.hpp"

#include <string>

#include "waybox/frame.hpp"

using wb::FrameButton;
using wb::FrameMetrics;
using wb::FramePart;
using wb::Rect;

static FrameMetrics metrics() {
	FrameMetrics m;
	m.border = 2;
	m.titlebar = 24;
	m.button = 18;
	m.button_spacing = 2;
	m.title_pad = 4;
	m.corner = 10;
	return m;
}

WB_TEST(insets_and_frame_size) {
	FrameMetrics m = metrics();
	wb::FrameInsets in = wb::frame_insets(m);
	WB_CHECK(in.top == m.border + m.titlebar);  /* 26 */
	WB_CHECK(in.left == m.border);
	WB_CHECK(in.right == m.border);
	WB_CHECK(in.bottom == m.border);

	/* a 100x80 client => 100+2*2 wide, 80 + 26 + 2 tall */
	WB_CHECK(wb::frame_width(100, m) == 104);
	WB_CHECK(wb::frame_height(80, m) == 108);
}

WB_TEST(titlebar_and_buttons_layout) {
	FrameMetrics m = metrics();
	int outer_w = 200;
	Rect tb = wb::titlebar_rect(outer_w, m);
	WB_CHECK(tb.x == m.border && tb.y == m.border);
	WB_CHECK(tb.width == outer_w - 2 * m.border);
	WB_CHECK(tb.height == m.titlebar);

	std::vector<FrameButton> buttons = {FrameButton::Iconify,
			FrameButton::Maximize, FrameButton::Close};
	auto rects = wb::button_rects(outer_w, m, buttons);
	WB_CHECK(rects.size() == 3);
	/* right-aligned: the close (last) button's right edge is title_pad from the
	 * titlebar's right edge */
	int tb_right = tb.x + tb.width;
	WB_CHECK(rects[2].x + rects[2].width == tb_right - m.title_pad);
	/* buttons are ordered left-to-right with the configured spacing */
	WB_CHECK(rects[1].x + rects[1].width + m.button_spacing == rects[2].x);
	WB_CHECK(rects[0].x + rects[0].width + m.button_spacing == rects[1].x);
	/* all the same square size, vertically centred in the titlebar */
	for (const Rect &r : rects) {
		WB_CHECK(r.width == m.button && r.height == m.button);
		WB_CHECK(r.y == tb.y + (tb.height - m.button) / 2);
	}
}

WB_TEST(hit_test_corners_edges_titlebar_client) {
	FrameMetrics m = metrics();
	std::vector<FrameButton> buttons = {FrameButton::Iconify,
			FrameButton::Maximize, FrameButton::Close};
	int w = 200, h = 150;

	/* corners win over edges */
	WB_CHECK(wb::frame_part_at(0, 0, w, h, m, buttons).part ==
			FramePart::CornerTopLeft);
	WB_CHECK(wb::frame_part_at(w - 1, 0, w, h, m, buttons).part ==
			FramePart::CornerTopRight);
	WB_CHECK(wb::frame_part_at(0, h - 1, w, h, m, buttons).part ==
			FramePart::CornerBottomLeft);
	WB_CHECK(wb::frame_part_at(w - 1, h - 1, w, h, m, buttons).part ==
			FramePart::CornerBottomRight);

	/* a button in the titlebar (centre of the close button) */
	auto rects = wb::button_rects(w, m, buttons);
	wb::FrameHit close = wb::frame_part_at(rects[2].x + rects[2].width / 2,
			rects[2].y + rects[2].height / 2, w, h, m, buttons);
	WB_CHECK(close.part == FramePart::Button);
	WB_CHECK(close.button == 2);

	/* titlebar background (left side, away from corner + buttons) */
	WB_CHECK(wb::frame_part_at(40, m.border + m.titlebar / 2, w, h, m, buttons)
			.part == FramePart::Titlebar);

	/* left/right/bottom borders (mid-edge, away from corners) */
	WB_CHECK(wb::frame_part_at(0, h / 2, w, h, m, buttons).part ==
			FramePart::BorderLeft);
	WB_CHECK(wb::frame_part_at(w - 1, h / 2, w, h, m, buttons).part ==
			FramePart::BorderRight);
	WB_CHECK(wb::frame_part_at(w / 2, h - 1, w, h, m, buttons).part ==
			FramePart::BorderBottom);

	/* deep inside => the client area (not our decoration) */
	WB_CHECK(wb::frame_part_at(w / 2, h / 2, w, h, m, buttons).part ==
			FramePart::Client);
}

WB_TEST(resize_grab_margin) {
	FrameMetrics m = metrics();
	m.resize_grab = 6;
	/* The grab margin is input-only: it must NOT enlarge the layout insets, the
	 * frame size, or shift the titlebar (otherwise SSD windows float away from
	 * screen edges/panels). */
	wb::FrameInsets in = wb::frame_insets(m);
	WB_CHECK(in.left == m.border);
	WB_CHECK(in.top == m.border + m.titlebar);
	WB_CHECK(in.bottom == m.border);
	WB_CHECK(wb::frame_width(100, m) == 100 + 2 * m.border);
	Rect tb = wb::titlebar_rect(200, m);
	WB_CHECK(tb.x == m.border && tb.y == m.border);

	std::vector<FrameButton> buttons = {FrameButton::Iconify,
			FrameButton::Maximize, FrameButton::Close};
	int outer_w = 200, outer_h = 150;
	/* Points in the invisible margin are addressed with coordinates outside
	 * [0, outer): negative above/left, >= outer below/right. They resolve to a
	 * graspable border (the FrameView's grab rect makes them hit-testable). */
	WB_CHECK(wb::frame_part_at(-m.resize_grab + 1, outer_h / 2, outer_w, outer_h,
			m, buttons).part == FramePart::BorderLeft);
	WB_CHECK(wb::frame_part_at(outer_w / 2, -m.resize_grab + 1, outer_w, outer_h,
			m, buttons).part == FramePart::BorderTop);
	WB_CHECK(wb::frame_part_at(outer_w + m.resize_grab - 1, outer_h / 2, outer_w,
			outer_h, m, buttons).part == FramePart::BorderRight);
	WB_CHECK(wb::frame_part_at(outer_w / 2, outer_h + m.resize_grab - 1, outer_w,
			outer_h, m, buttons).part == FramePart::BorderBottom);
	/* A margin point near a corner is a diagonal resize. */
	WB_CHECK(wb::frame_part_at(-m.resize_grab + 1, -m.resize_grab + 1, outer_w,
			outer_h, m, buttons).part == FramePart::CornerTopLeft);
	/* Deep inside is still the client. */
	WB_CHECK(wb::frame_part_at(outer_w / 2, outer_h / 2, outer_w, outer_h, m,
			buttons).part == FramePart::Client);
}

WB_TEST(frame_part_cursor_names) {
	WB_CHECK(std::string(wb::frame_part_cursor(FramePart::BorderTop)) == "n-resize");
	WB_CHECK(std::string(wb::frame_part_cursor(FramePart::BorderLeft)) == "w-resize");
	WB_CHECK(std::string(wb::frame_part_cursor(FramePart::CornerTopLeft)) == "nw-resize");
	WB_CHECK(std::string(wb::frame_part_cursor(FramePart::CornerBottomRight)) == "se-resize");
	WB_CHECK(wb::frame_part_cursor(FramePart::Titlebar) == nullptr);
	WB_CHECK(wb::frame_part_cursor(FramePart::Button) == nullptr);
	WB_CHECK(wb::frame_part_cursor(FramePart::Client) == nullptr);
}

WB_TEST(resize_edges_match_wlr_bits) {
	/* The model's edge bits equal WLR_EDGE_* (TOP=1, BOTTOM=2, LEFT=4, RIGHT=8). */
	WB_CHECK(wb::frame_part_resize_edges(FramePart::BorderTop) == 1);
	WB_CHECK(wb::frame_part_resize_edges(FramePart::BorderBottom) == 2);
	WB_CHECK(wb::frame_part_resize_edges(FramePart::BorderLeft) == 4);
	WB_CHECK(wb::frame_part_resize_edges(FramePart::BorderRight) == 8);
	WB_CHECK(wb::frame_part_resize_edges(FramePart::CornerTopLeft) == (1 | 4));
	WB_CHECK(wb::frame_part_resize_edges(FramePart::CornerBottomRight) == (2 | 8));
	WB_CHECK(wb::frame_part_resize_edges(FramePart::Titlebar) == 0);
	WB_CHECK(wb::frame_part_resize_edges(FramePart::Client) == 0);
}
