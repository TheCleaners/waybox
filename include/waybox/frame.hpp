#ifndef WB_FRAME_HPP
#define WB_FRAME_HPP

#include <vector>

#include "waybox/geometry.hpp"

namespace wb {

/*
 * Server-side decoration geometry (pure).
 *
 * Computes the layout of a window frame — titlebar, borders, and titlebar
 * buttons — and hit-tests a point against it, all without wlroots/Cairo so it
 * is unit-tested. The renderer (decoration scene subtree) and the input router
 * consume these results. "Outer" coordinates are frame-local (0,0 = top-left of
 * the whole decorated frame); the client surface sits inside the insets.
 */

/* Titlebar buttons, in the order Openbox names them. Order on the bar is
 * configurable; this is just the identity. */
enum class FrameButton {
	Iconify,
	Maximize,
	Close,
};

/* Which part of the frame is under a point — drives the cursor shape and what a
 * press does (move / resize / button action / pass to client). */
enum class FramePart {
	None,
	Client,       /* inside the client area: not our decoration */
	Titlebar,     /* drag to move */
	Button,       /* a titlebar button (see FrameHit::button) */
	BorderTop,
	BorderBottom,
	BorderLeft,
	BorderRight,
	CornerTopLeft,
	CornerTopRight,
	CornerBottomLeft,
	CornerBottomRight,
};

/* Pixel metrics of a frame (logical px), derived from the theme/FrameStyle. */
struct FrameMetrics {
	int border = 1;           /* side + bottom border thickness */
	int titlebar = 24;        /* titlebar height (top), 0 hides the titlebar */
	int button = 18;          /* square button box edge */
	int button_spacing = 2;   /* gap between buttons */
	int title_pad = 4;        /* horizontal padding inside the titlebar */
	int corner = 10;          /* corner grab reach for diagonal resize */
	int resize_grab = 0;      /* invisible resize margin outside the visible
	                           * frame; widens the border/corner grab area */
};

/* Decoration thickness on each side; the client area is the frame inset by it. */
struct FrameInsets {
	int top = 0;     /* border + titlebar */
	int right = 0;
	int bottom = 0;
	int left = 0;
};

FrameInsets frame_insets(const FrameMetrics &m);

/* Outer frame size for a client of size (client_w, client_h). */
int frame_width(int client_w, const FrameMetrics &m);
int frame_height(int client_h, const FrameMetrics &m);

/* The titlebar rectangle in frame-local (outer) coordinates. */
Rect titlebar_rect(int outer_w, const FrameMetrics &m);

/*
 * Button rectangles in frame-local coordinates, right-aligned in the titlebar
 * in the given order (first element leftmost of the group). `buttons` lists the
 * buttons present; the returned rects are parallel to it.
 */
std::vector<Rect> button_rects(int outer_w, const FrameMetrics &m,
		const std::vector<FrameButton> &buttons);

/* The result of hit-testing a frame-local point. */
struct FrameHit {
	FramePart part = FramePart::None;
	int button = -1;  /* index into the buttons list when part == Button */
};

/*
 * Hit-test a frame-local point (ox, oy) against a frame of outer size
 * (outer_w, outer_h). Corners win over edges; buttons win over the titlebar.
 * A point inside the client area returns FramePart::Client.
 */
FrameHit frame_part_at(int ox, int oy, int outer_w, int outer_h,
		const FrameMetrics &m, const std::vector<FrameButton> &buttons);

/* Map resize parts to wlroots-style edge bitmasks is done at the call site;
 * frame_part_resize_edges yields the WLR_EDGE_* combination for a part (0 for
 * non-resizable parts) using the standard bit values, kept here as plain ints
 * so the model stays wlroots-free. */
enum {
	WB_FRAME_EDGE_TOP = 1,
	WB_FRAME_EDGE_BOTTOM = 2,
	WB_FRAME_EDGE_LEFT = 4,
	WB_FRAME_EDGE_RIGHT = 8,
};
int frame_part_resize_edges(FramePart part);

/* The xcursor name (e.g. "se-resize", "left_side") for a frame part's resize
 * affordance, or nullptr for non-resize parts (titlebar/button/client). */
const char *frame_part_cursor(FramePart part);

}  // namespace wb

#endif /* WB_FRAME_HPP */
