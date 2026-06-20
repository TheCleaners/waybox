#include "wb_test.hpp"

#include "waybox/geometry.hpp"

using wb::apply_strut;
using wb::constrain_to_usable;
using wb::Rect;
using wb::Strut;
using wb::strut_between;

WB_TEST(empty_detects_zero_area) {
	WB_CHECK(wb::empty(Rect{0, 0, 0, 10}));
	WB_CHECK(wb::empty(Rect{0, 0, 10, 0}));
	WB_CHECK(wb::empty(Rect{0, 0, -5, 10}));
	WB_CHECK(!wb::empty(Rect{0, 0, 1, 1}));
}

WB_TEST(apply_strut_insets_and_clamps) {
	Rect area{0, 0, 100, 100};
	/* a 30px top panel */
	WB_CHECK(apply_strut(area, Strut{0, 30, 0, 0}) == (Rect{0, 30, 100, 70}));
	/* margins on every edge */
	WB_CHECK(apply_strut(area, Strut{5, 10, 5, 10}) == (Rect{5, 10, 90, 80}));
	/* over-large strut clamps size to zero, never negative */
	Rect r = apply_strut(area, Strut{80, 0, 80, 0});
	WB_CHECK(r.width == 0);
	WB_CHECK(r.x == 80);
}

WB_TEST(apply_strut_respects_origin) {
	Rect area{200, 100, 50, 50};
	WB_CHECK(apply_strut(area, Strut{0, 20, 0, 0}) == (Rect{200, 120, 50, 30}));
}

WB_TEST(strut_between_is_inverse_of_apply) {
	Rect outer{0, 0, 100, 100};
	Strut s{5, 30, 7, 12};
	Rect inner = apply_strut(outer, s);
	WB_CHECK(strut_between(outer, inner) == s);
}

WB_TEST(strut_between_clamps_negative) {
	Rect outer{0, 0, 100, 100};
	/* inner larger than outer -> no negative reservations */
	Rect inner{-10, -10, 120, 120};
	WB_CHECK(strut_between(outer, inner) == (Strut{0, 0, 0, 0}));
}

WB_TEST(constrain_pushes_off_reserved_top_edge) {
	Rect outer{0, 0, 100, 100};
	Rect usable{0, 30, 100, 70};  /* top panel reserves 30px */
	/* a window above the panel is pushed down to the usable top */
	WB_CHECK(constrain_to_usable(Rect{10, 0, 40, 40}, outer, usable).y == 30);
	/* a window already below is untouched */
	WB_CHECK(constrain_to_usable(Rect{10, 50, 40, 40}, outer, usable).y == 50);
}

WB_TEST(constrain_leaves_free_edges_unconstrained) {
	Rect outer{0, 0, 100, 100};
	Rect usable{0, 30, 100, 70};  /* only the top edge is reserved */
	/* window dragged off the bottom (a free edge) stays put */
	Rect box{10, 90, 40, 40};
	Rect out = constrain_to_usable(box, outer, usable);
	WB_CHECK(out.y == 90);
	/* off the left (free) edge stays put too */
	WB_CHECK(constrain_to_usable(Rect{-20, 50, 40, 40}, outer, usable).x == -20);
}

WB_TEST(constrain_handles_reserved_right_and_bottom) {
	Rect outer{0, 0, 100, 100};
	Rect usable{0, 0, 80, 80};  /* right 20px and bottom 20px reserved */
	Rect out = constrain_to_usable(Rect{90, 90, 30, 30}, outer, usable);
	WB_CHECK(out.x == 80 - 30);
	WB_CHECK(out.y == 80 - 30);
}

WB_TEST(constrain_respects_output_origin) {
	/* second monitor at x=1920; usable inset 40px on the left (a left panel) */
	Rect outer{1920, 0, 1000, 1000};
	Rect usable{1960, 0, 960, 1000};
	WB_CHECK(constrain_to_usable(Rect{1920, 10, 50, 50}, outer, usable).x == 1960);
	/* a window already past the panel is untouched */
	WB_CHECK(constrain_to_usable(Rect{2000, 10, 50, 50}, outer, usable).x == 2000);
}
