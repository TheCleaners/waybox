#include "wb_test.hpp"

#include <vector>

#include "waybox/edge_tiling.hpp"

using wb::Rect;
using wb::TileZone;
using wb::edge_zone;
using wb::tile_rect;

static const Rect kOut{0, 0, 1200, 600};  /* corners: 100 px high, 200 px wide */
static const std::vector<Rect> kOne{kOut};

WB_TEST(zones_on_a_single_monitor) {
	WB_CHECK(edge_zone(3, 300, kOut, kOne, 10) == TileZone::Left);
	WB_CHECK(edge_zone(1195, 300, kOut, kOne, 10) == TileZone::Right);
	WB_CHECK(edge_zone(600, 2, kOut, kOne, 10) == TileZone::Top);
	WB_CHECK(edge_zone(3, 20, kOut, kOne, 10) == TileZone::TopLeft);
	WB_CHECK(edge_zone(1195, 590, kOut, kOne, 10) == TileZone::BottomRight);
	WB_CHECK(edge_zone(50, 2, kOut, kOne, 10) == TileZone::TopLeft);
	WB_CHECK(edge_zone(1150, 597, kOut, kOne, 10) == TileZone::BottomRight);
	/* The middle of the bottom edge is not a zone. */
	WB_CHECK(edge_zone(600, 597, kOut, kOne, 10) == TileZone::None);
	/* Away from the edges, outside the monitor, or no range: nothing. */
	WB_CHECK(edge_zone(600, 300, kOut, kOne, 10) == TileZone::None);
	WB_CHECK(edge_zone(-5, 300, kOut, kOne, 10) == TileZone::None);
	WB_CHECK(edge_zone(3, 300, kOut, kOne, 0) == TileZone::None);
}

WB_TEST(inner_edges_between_monitors_are_not_zones) {
	const Rect a{0, 0, 1200, 600}, b{1200, 0, 800, 600};
	const std::vector<Rect> both{a, b};
	WB_CHECK(edge_zone(1195, 300, a, both, 10) == TileZone::None);  /* a's right */
	WB_CHECK(edge_zone(1203, 300, b, both, 10) == TileZone::None);  /* b's left */
	WB_CHECK(edge_zone(3, 300, a, both, 10) == TileZone::Left);     /* outer */
	WB_CHECK(edge_zone(1995, 300, b, both, 10) == TileZone::Right);
	/* A shorter neighbour: the part of the edge it doesn't continue is outer. */
	const Rect c{1200, 0, 800, 300};
	const std::vector<Rect> ac{a, c};
	WB_CHECK(edge_zone(1195, 100, a, ac, 10) == TileZone::None);
	WB_CHECK(edge_zone(1195, 450, a, ac, 10) == TileZone::Right);
}

WB_TEST(rects_split_the_area) {
	const Rect area{10, 30, 1001, 501};  /* odd sizes: no pixel lost */
	Rect l = tile_rect(TileZone::Left, area), r = tile_rect(TileZone::Right, area);
	WB_CHECK(l.x == 10 && l.y == 30 && l.height == 501);
	WB_CHECK(r.x == l.x + l.width && l.width + r.width == 1001);
	WB_CHECK(tile_rect(TileZone::Top, area) == area);
	Rect tl = tile_rect(TileZone::TopLeft, area), br = tile_rect(TileZone::BottomRight, area);
	WB_CHECK(tl.x == 10 && tl.y == 30);
	WB_CHECK(br.x == tl.x + tl.width && br.y == tl.y + tl.height);
	WB_CHECK(tl.width + br.width == 1001 && tl.height + br.height == 501);
}

WB_TEST(direction_names) {
	WB_CHECK(wb::tile_zone_from_name("left") == TileZone::Left);
	WB_CHECK(wb::tile_zone_from_name("up") == TileZone::Top);
	WB_CHECK(wb::tile_zone_from_name("down-right") == TileZone::BottomRight);
	WB_CHECK(!wb::tile_zone_from_name("down"));
	WB_CHECK(!wb::tile_zone_from_name("sideways"));
	WB_CHECK(wb::tile_zone_name(TileZone::TopLeft) == "top-left");
}
