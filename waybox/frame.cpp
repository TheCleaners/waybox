/*
 * Pure server-side decoration geometry. No wlroots/Cairo; unit-tested.
 */
#include "waybox/frame.hpp"

namespace wb {

FrameInsets frame_insets(const FrameMetrics &m) {
	FrameInsets in;
	in.top = m.border + m.titlebar;
	in.left = m.border;
	in.right = m.border;
	in.bottom = m.border;
	return in;
}

int frame_width(int client_w, const FrameMetrics &m) {
	FrameInsets in = frame_insets(m);
	return client_w + in.left + in.right;
}

int frame_height(int client_h, const FrameMetrics &m) {
	FrameInsets in = frame_insets(m);
	return client_h + in.top + in.bottom;
}

Rect titlebar_rect(int outer_w, const FrameMetrics &m) {
	/* Just inside the top/side borders. */
	return Rect{m.border, m.border, outer_w - 2 * m.border, m.titlebar};
}

std::vector<Rect> button_rects(int outer_w, const FrameMetrics &m,
		const std::vector<FrameButton> &buttons) {
	std::vector<Rect> rects;
	rects.reserve(buttons.size());
	if (m.titlebar <= 0)
		return rects;

	Rect tb = titlebar_rect(outer_w, m);
	int by = tb.y + (tb.height - m.button) / 2;  /* vertically centred */
	/* Right-aligned group: the last button sits at the far right (minus pad),
	 * earlier buttons to its left. Compute left-to-right so rects parallel the
	 * input order. */
	int n = static_cast<int>(buttons.size());
	int group_w = n * m.button + (n > 0 ? (n - 1) * m.button_spacing : 0);
	int x = tb.x + tb.width - m.title_pad - group_w;
	for (int i = 0; i < n; ++i) {
		rects.push_back(Rect{x, by, m.button, m.button});
		x += m.button + m.button_spacing;
	}
	return rects;
}

static bool in_rect(int x, int y, const Rect &r) {
	return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

FrameHit frame_part_at(int ox, int oy, int outer_w, int outer_h,
		const FrameMetrics &m, const std::vector<FrameButton> &buttons) {
	FrameInsets in = frame_insets(m);

	/* Corners first (diagonal resize), using the corner grab reach. */
	bool near_left = ox < m.corner;
	bool near_right = ox >= outer_w - m.corner;
	bool near_top = oy < m.corner;
	bool near_bottom = oy >= outer_h - m.corner;
	if (near_top && near_left)
		return {FramePart::CornerTopLeft, -1};
	if (near_top && near_right)
		return {FramePart::CornerTopRight, -1};
	if (near_bottom && near_left)
		return {FramePart::CornerBottomLeft, -1};
	if (near_bottom && near_right)
		return {FramePart::CornerBottomRight, -1};

	/* Buttons (win over the titlebar background). */
	std::vector<Rect> brects = button_rects(outer_w, m, buttons);
	for (size_t i = 0; i < brects.size(); ++i) {
		if (in_rect(ox, oy, brects[i]))
			return {FramePart::Button, static_cast<int>(i)};
	}

	/* Titlebar (drag to move). */
	if (m.titlebar > 0 && in_rect(ox, oy, titlebar_rect(outer_w, m)))
		return {FramePart::Titlebar, -1};

	/* Edges. */
	if (oy < in.top - m.titlebar)  /* the top border above the titlebar */
		return {FramePart::BorderTop, -1};
	if (oy >= outer_h - in.bottom)
		return {FramePart::BorderBottom, -1};
	if (ox < in.left)
		return {FramePart::BorderLeft, -1};
	if (ox >= outer_w - in.right)
		return {FramePart::BorderRight, -1};

	return {FramePart::Client, -1};
}

int frame_part_resize_edges(FramePart part) {
	switch (part) {
	case FramePart::BorderTop:
		return WB_FRAME_EDGE_TOP;
	case FramePart::BorderBottom:
		return WB_FRAME_EDGE_BOTTOM;
	case FramePart::BorderLeft:
		return WB_FRAME_EDGE_LEFT;
	case FramePart::BorderRight:
		return WB_FRAME_EDGE_RIGHT;
	case FramePart::CornerTopLeft:
		return WB_FRAME_EDGE_TOP | WB_FRAME_EDGE_LEFT;
	case FramePart::CornerTopRight:
		return WB_FRAME_EDGE_TOP | WB_FRAME_EDGE_RIGHT;
	case FramePart::CornerBottomLeft:
		return WB_FRAME_EDGE_BOTTOM | WB_FRAME_EDGE_LEFT;
	case FramePart::CornerBottomRight:
		return WB_FRAME_EDGE_BOTTOM | WB_FRAME_EDGE_RIGHT;
	default:
		return 0;
	}
}

}  // namespace wb
