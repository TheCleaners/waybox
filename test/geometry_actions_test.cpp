#include "wb_test.hpp"

#include "waybox/geometry_actions.hpp"

using wb::EdgeDir;
using wb::Rect;

namespace {

/* A 1000x800 area at (0, 30) (a panel above it); the window 200x100. */
const Rect kArea{0, 30, 1000, 800};
const Rect kWin{100, 130, 200, 100};

bool same(const Rect &a, const Rect &b) {
	return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

}  // namespace

WB_TEST(move_resize_to_positions_and_sizes) {
	using wb::move_resize_to;
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"x", "10"}, {"y", "20"}}), {10, 50, 200, 100}));
	/* center, negatives from the far edge, percentages, current. */
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"x", "center"}, {"y", "center"}}),
			{400, 380, 200, 100}));
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"x", "-0"}, {"y", "-10"}}), {800, 720, 200, 100}));
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"width", "50%"}, {"height", "25%"}}),
			{100, 130, 500, 200}));
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"x", "current"}, {"width", "current"}}), kWin));
	/* The size first, so "center" centers the new size. */
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"width", "400"}, {"x", "center"}}),
			{300, 130, 400, 100}));
	/* Nonsense: left as it was. */
	WB_CHECK(same(move_resize_to(kWin, kArea, {{"x", "abc"}, {"width", "-5"}}), kWin));
}

WB_TEST(move_and_resize_relative) {
	WB_CHECK(same(wb::move_relative(kWin, {{"x", "-50"}, {"y", "25"}}), {50, 155, 200, 100}));
	WB_CHECK(same(wb::resize_relative(kWin, {{"left", "10"}, {"right", "20"}, {"top", "5"},
			{"bottom", "-50"}}), {90, 125, 230, 55}));
	/* Openbox's older <x> <y>: the right and bottom edges. */
	WB_CHECK(same(wb::resize_relative(kWin, {{"x", "30"}, {"y", "40"}}), {100, 130, 230, 140}));
	WB_CHECK(same(wb::move_to_center(kWin, kArea), {400, 380, 200, 100}));
}

WB_TEST(edge_names) {
	WB_CHECK(wb::edge_dir_from_name("north") == EdgeDir::North);
	WB_CHECK(wb::edge_dir_from_name("left") == EdgeDir::West);
	WB_CHECK(!wb::edge_dir_from_name("sideways").has_value());
}

WB_TEST(move_to_edge_stops_at_windows_and_the_area) {
	/* A window alongside to the right, at x 500..700. */
	std::vector<Rect> others = {{500, 100, 200, 300}, {900, 600, 50, 50}};
	Rect r = wb::move_to_edge(kWin, kArea, others, EdgeDir::East);
	WB_CHECK(same(r, {300, 130, 200, 100}));            /* touches its left edge */
	r = wb::move_to_edge(r, kArea, others, EdgeDir::East);
	WB_CHECK(same(r, {500, 130, 200, 100}));            /* then aligns with its right */
	WB_CHECK(same(wb::move_to_edge(kWin, kArea, {}, EdgeDir::North), {100, 30, 200, 100}));
	WB_CHECK(same(wb::move_to_edge(kWin, kArea, {}, EdgeDir::South), {100, 730, 200, 100}));
	/* Not alongside (below it): no obstacle. */
	WB_CHECK(same(wb::move_to_edge(kWin, kArea, {{900, 600, 50, 50}}, EdgeDir::East),
			{800, 130, 200, 100}));
}

WB_TEST(grow_shrink_and_fill) {
	std::vector<Rect> others = {{500, 100, 200, 300}};
	/* Grows to the window alongside; touching it, no further. */
	Rect g = wb::grow_to_edge(kWin, kArea, others, EdgeDir::East);
	WB_CHECK(same(g, {100, 130, 400, 100}));
	WB_CHECK(same(wb::grow_to_edge(g, kArea, others, EdgeDir::East), g));
	WB_CHECK(same(wb::grow_to_edge(kWin, kArea, {}, EdgeDir::West), {0, 130, 300, 100}));
	/* Shrinks back to the nearest edge inside it, not below its minimum. */
	Rect wide{100, 130, 700, 100};
	WB_CHECK(same(wb::shrink_to_edge(wide, others, EdgeDir::East), {100, 130, 600, 100}));
	WB_CHECK(same(wb::shrink_to_edge(kWin, {}, EdgeDir::East), kWin));
	/* Fill: every side out, stopping at the window on the right. */
	WB_CHECK(same(wb::grow_to_fill(kWin, kArea, others), {0, 30, 500, 800}));
}
