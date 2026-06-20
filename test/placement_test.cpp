#include "wb_test.hpp"

#include "waybox/placement.hpp"

#include <vector>

using wb::place_center;
using wb::place_smart;
using wb::place_under_mouse;
using wb::PlacementPolicy;
using wb::Rect;

WB_TEST(policy_names_parse) {
	WB_CHECK(wb::placement_policy_from_name("Smart") == PlacementPolicy::Smart);
	WB_CHECK(wb::placement_policy_from_name("Center") == PlacementPolicy::Center);
	WB_CHECK(wb::placement_policy_from_name("UnderMouse") == PlacementPolicy::UnderMouse);
	WB_CHECK(!wb::placement_policy_from_name("nope").has_value());
}

WB_TEST(smart_empty_area_uses_top_left) {
	Rect area{0, 0, 800, 600};
	std::vector<Rect> none;
	WB_CHECK(place_smart(area, 100, 80, none) == (Rect{0, 0, 100, 80}));
}

WB_TEST(smart_places_next_to_existing_without_overlap) {
	Rect area{0, 0, 800, 600};
	/* one 100x80 window at the top-left */
	std::vector<Rect> windows{{0, 0, 100, 80}};
	Rect r = place_smart(area, 100, 80, windows);
	/* the new window must not overlap the existing one */
	for (const Rect &w : windows) {
		bool disjoint = r.x >= w.x + w.width || w.x >= r.x + r.width ||
				r.y >= w.y + w.height || w.y >= r.y + r.height;
		WB_CHECK(disjoint);
	}
	/* top-left bias: it should land just to the right of the first window */
	WB_CHECK(r == (Rect{100, 0, 100, 80}));
}

WB_TEST(smart_respects_origin_offset) {
	Rect area{200, 100, 400, 300};  /* e.g. a margin-inset usable area */
	std::vector<Rect> none;
	WB_CHECK(place_smart(area, 50, 50, none) == (Rect{200, 100, 50, 50}));
}

WB_TEST(smart_falls_back_when_window_larger_than_area) {
	Rect area{0, 0, 100, 100};
	std::vector<Rect> none;
	/* a 200x200 window cannot fit; fall back to the area's top-left */
	WB_CHECK(place_smart(area, 200, 200, none) == (Rect{0, 0, 200, 200}));
}

WB_TEST(smart_minimizes_overlap_when_full) {
	/* area only fits one window; a second must overlap. It should still pick a
	 * candidate origin (top-left here) rather than going out of bounds. */
	Rect area{0, 0, 100, 100};
	std::vector<Rect> windows{{0, 0, 100, 100}};
	Rect r = place_smart(area, 100, 100, windows);
	WB_CHECK(r == (Rect{0, 0, 100, 100}));
}

WB_TEST(smart_cascades_when_overlap_unavoidable) {
	/* Two 600x600 windows cannot tile in 1000x1000 without overlap; rather
	 * than stack exactly, the second cascades by the step (32) keyed on the
	 * existing window count. */
	Rect area{0, 0, 1000, 1000};
	std::vector<Rect> windows{{0, 0, 600, 600}};
	WB_CHECK(place_smart(area, 600, 600, windows) == (Rect{32, 32, 600, 600}));
	/* a third cascades twice as far */
	std::vector<Rect> two{{0, 0, 600, 600}, {32, 32, 600, 600}};
	WB_CHECK(place_smart(area, 600, 600, two) == (Rect{64, 64, 600, 600}));
}

WB_TEST(center_centers_within_area) {
	Rect area{0, 0, 800, 600};
	WB_CHECK(place_center(area, 200, 100) == (Rect{300, 250, 200, 100}));
	Rect area2{100, 50, 400, 300};
	WB_CHECK(place_center(area2, 100, 100) == (Rect{250, 150, 100, 100}));
}

WB_TEST(under_mouse_centers_on_cursor_and_clamps) {
	Rect area{0, 0, 800, 600};
	/* cursor mid-screen -> window centred on it */
	WB_CHECK(place_under_mouse(area, 100, 100, 400, 300) == (Rect{350, 250, 100, 100}));
	/* cursor near a corner -> clamped into the area */
	WB_CHECK(place_under_mouse(area, 100, 100, 5, 5) == (Rect{0, 0, 100, 100}));
	Rect r = place_under_mouse(area, 100, 100, 799, 599);
	WB_CHECK(r.x == 700);
	WB_CHECK(r.y == 500);
}

WB_TEST(under_mouse_handles_window_larger_than_area) {
	Rect area{0, 0, 100, 100};
	/* window larger than area must not produce an inverted clamp */
	Rect r = place_under_mouse(area, 200, 200, 50, 50);
	WB_CHECK(r.x == 0);
	WB_CHECK(r.y == 0);
}

WB_TEST(snap_disabled_when_distance_zero) {
	Rect area{0, 0, 800, 600};
	std::vector<Rect> none;
	WB_CHECK(wb::snap_move(Rect{3, 4, 50, 50}, area, none, 0) == (Rect{3, 4, 50, 50}));
}

WB_TEST(snap_to_area_edges) {
	Rect area{0, 0, 800, 600};
	std::vector<Rect> none;
	/* near the top-left -> snaps both edges to 0 */
	WB_CHECK(wb::snap_move(Rect{4, 3, 50, 50}, area, none, 8) == (Rect{0, 0, 50, 50}));
	/* near the bottom-right -> right/bottom edges snap to the area edges */
	Rect r = wb::snap_move(Rect{746, 548, 50, 50}, area, none, 8);
	WB_CHECK(r.x == 750);  /* 800 - 50 */
	WB_CHECK(r.y == 550);  /* 600 - 50 */
}

WB_TEST(snap_far_from_edges_is_noop) {
	Rect area{0, 0, 800, 600};
	std::vector<Rect> none;
	WB_CHECK(wb::snap_move(Rect{400, 300, 50, 50}, area, none, 8) == (Rect{400, 300, 50, 50}));
}

WB_TEST(snap_to_adjacent_window_edge) {
	Rect area{0, 0, 800, 600};
	/* an existing window occupying the left half, overlapping vertically */
	std::vector<Rect> windows{{0, 0, 400, 600}};
	/* a window dragged near the right edge of the existing one snaps flush */
	Rect r = wb::snap_move(Rect{404, 100, 200, 200}, area, windows, 8);
	WB_CHECK(r.x == 400);  /* left edge snaps to the other window's right edge */
	WB_CHECK(r.y == 100);  /* y far from any edge, unchanged */
}

WB_TEST(snap_skips_window_without_perpendicular_overlap) {
	Rect area{0, 0, 800, 600};
	/* existing window only in the top band; the dragged window is far below it,
	 * so there is no vertical overlap and its right edge should NOT pull us. */
	std::vector<Rect> windows{{0, 0, 400, 50}};
	Rect r = wb::snap_move(Rect{404, 400, 200, 100}, area, windows, 8);
	WB_CHECK(r.x == 404);  /* no snap: windows don't overlap vertically */
}
