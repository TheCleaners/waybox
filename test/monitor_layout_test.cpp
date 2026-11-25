#include "wb_test.hpp"

#include "waybox/monitor_layout.hpp"

using wb::LayoutBox;

WB_TEST(logical_sizes) {
	WB_CHECK((wb::logical_size(3840, 2160, 2, "normal") == std::pair{1920, 1080}));
	WB_CHECK((wb::logical_size(3840, 2160, 1.5, "") == std::pair{2560, 1440}));
	WB_CHECK((wb::logical_size(1920, 1080, 1, "90") == std::pair{1080, 1920}));
	WB_CHECK((wb::logical_size(1920, 1080, 1, "flipped-270") == std::pair{1080, 1920}));
	WB_CHECK((wb::logical_size(1920, 1080, 1, "180") == std::pair{1920, 1080}));
	WB_CHECK((wb::logical_size(1920, 1080, 1, "flipped") == std::pair{1920, 1080}));
	WB_CHECK((wb::logical_size(1920, 1080, 0, "normal") == std::pair{1920, 1080}));  /* bad scale */
	WB_CHECK((wb::logical_size(0, 0, 1, "normal") == std::pair{1, 1}));
}

WB_TEST(overlap_and_touch) {
	LayoutBox a{"a", 0, 0, 100, 100}, b{"b", 100, 0, 50, 50};
	WB_CHECK(!wb::boxes_overlap(a, b) && wb::boxes_touch(a, b));  /* edge */
	b.x = 100; b.y = 100;
	WB_CHECK(wb::boxes_touch(a, b));                               /* corner */
	b.x = 101;
	WB_CHECK(!wb::boxes_touch(a, b));                              /* a gap */
	b.x = 99; b.y = 0;
	WB_CHECK(wb::boxes_overlap(a, b) && !wb::boxes_touch(a, b));
	b.y = 101; b.x = 0;
	WB_CHECK(!wb::boxes_touch(a, b));
}

WB_TEST(snapping) {
	std::vector<LayoutBox> boxes = {{"a", 0, 0, 1920, 1080}, {"b", 0, 0, 1280, 1024}};
	/* Near a's right edge: its left edge goes onto it; tops line up. */
	WB_CHECK((wb::snap_position(boxes, 1, 1925, 6, 10) == std::pair{1920, 0}));
	/* Bottoms line up: 1080 - 1024. */
	WB_CHECK((wb::snap_position(boxes, 1, 1920, 50, 10) == std::pair{1920, 56}));
	/* Far from anything: as dragged. */
	WB_CHECK((wb::snap_position(boxes, 1, 2400, 300, 10) == std::pair{2400, 300}));
}

WB_TEST(placing) {
	std::vector<LayoutBox> boxes = {{"a", 0, 0, 1920, 1080}, {"b", 1920, 0, 1280, 1024}};
	/* Where dropped, if it touches and overlaps nothing. */
	WB_CHECK((wb::place_box(boxes, 1, 1920, 200) == std::pair{1920, 200}));
	/* Overlapping: refused, put beside instead (the nearest place). */
	WB_CHECK((wb::place_box(boxes, 1, 1800, 0) == std::pair{1920, 0}));
	/* A gap: closed. */
	WB_CHECK((wb::place_box(boxes, 1, 2100, 0) == std::pair{1920, 0}));
	/* Below, a bit off: under a, slid along. */
	WB_CHECK((wb::place_box(boxes, 1, 300, 1200) == std::pair{300, 1080}));
	/* Far down to the right: at a's corner. */
	WB_CHECK((wb::place_box(boxes, 1, 5000, 5000) == std::pair{1920, 1080}));
	/* Alone: anywhere. */
	std::vector<LayoutBox> one = {{"a", 0, 0, 10, 10}};
	WB_CHECK((wb::place_box(one, 0, 77, 5) == std::pair{77, 5}));
	/* Three: never onto the third one. */
	std::vector<LayoutBox> three = {{"a", 0, 0, 100, 100}, {"b", 100, 0, 100, 100},
			{"c", 300, 0, 100, 100}};
	auto p = wb::place_box(three, 2, 150, 0);
	WB_CHECK(p.has_value());
	LayoutBox c = three[2];
	c.x = p->first;
	c.y = p->second;
	WB_CHECK(!wb::boxes_overlap(c, three[0]) && !wb::boxes_overlap(c, three[1]));
	WB_CHECK(wb::boxes_touch(c, three[0]) || wb::boxes_touch(c, three[1]));
}

WB_TEST(normalizing_and_positions) {
	std::vector<LayoutBox> boxes = {{"a", -1280, 200, 1280, 1024}, {"b", 0, 0, 1920, 1080}};
	wb::normalize_layout(boxes);
	WB_CHECK(boxes[0].x == 0 && boxes[0].y == 200 && boxes[1].x == 1280 && boxes[1].y == 0);
	WB_CHECK((wb::parse_position("1920,0") == std::pair{1920, 0}));
	WB_CHECK((wb::parse_position("-5,+7") == std::pair{-5, 7}));
	WB_CHECK(!wb::parse_position("1920"));
	WB_CHECK(!wb::parse_position("1920, 0"));
	WB_CHECK(!wb::parse_position(",0"));
	WB_CHECK(!wb::parse_position("99999999999,0"));
	WB_CHECK(!wb::parse_position("1x,0"));
}

WB_TEST(monitor_captions) {
	/* Make and model; the make not said twice. */
	WB_CHECK(wb::monitor_caption("Dell Inc.", "DELL U2723QE", "", "DP-3") == "DELL U2723QE");
	WB_CHECK(wb::monitor_caption("LG Electronics", "LG ULTRAGEAR", "", "DP-1") == "LG ULTRAGEAR");
	WB_CHECK(wb::monitor_caption("BOE", "0x0BCA", "", "eDP-1") == "BOE 0x0BCA");
	WB_CHECK(wb::monitor_caption("Samsung Electric Company", "", "", "HDMI-A-1") ==
			"Samsung Electric Company");
	WB_CHECK(wb::monitor_caption("", "U28E590", "", "DP-2") == "U28E590");
	/* No make or model: the description, without its "(NAME)". */
	WB_CHECK(wb::monitor_caption("", "", "Dell Inc. DELL U2723QE 5XYZ123 (DP-3)", "DP-3") ==
			"Dell Inc. DELL U2723QE 5XYZ123");
	WB_CHECK(wb::monitor_caption("", "", "Headless output 1", "HEADLESS-1") == "Headless output 1");
	WB_CHECK(wb::monitor_caption("", "", "Something (DP-1)", "DP-2") == "Something (DP-1)");
	WB_CHECK(wb::monitor_caption("", "", "", "DP-2").empty());
	/* Control characters, tabs, newlines, runs of spaces. */
	WB_CHECK(wb::monitor_caption("Ac\tme\n", "  X\x01Y  ", "", "DP-1") == "Ac me X Y");
	/* Long: cut at a character, not inside one, with an ellipsis. */
	std::string long_model(300, 'W');
	std::string c = wb::monitor_caption("", long_model, "", "DP-1", 40);
	WB_CHECK(c.size() <= 40 + 3 && c.rfind("…") == c.size() - 3);
	std::string accents;
	for (int i = 0; i < 60; ++i)
		accents += "é";  /* 2 bytes each */
	c = wb::monitor_caption("", accents, "", "DP-1", 41);
	WB_CHECK(c.size() == 40 + 3);  /* 20 whole characters, then … */
}

WB_TEST(transforms_split_and_joined) {
	using P = std::pair<std::string, bool>;
	WB_CHECK(wb::split_transform("") == P("", false));
	WB_CHECK(wb::split_transform("normal") == P("normal", false));
	WB_CHECK(wb::split_transform("270") == P("270", false));
	WB_CHECK(wb::split_transform("flipped") == P("normal", true));
	WB_CHECK(wb::split_transform("flipped-90") == P("90", true));
	WB_CHECK(!wb::split_transform("flipped-normal"));
	WB_CHECK(!wb::split_transform("flipped-"));
	WB_CHECK(!wb::split_transform("45"));
	WB_CHECK(!wb::split_transform("rotate-90"));
	WB_CHECK(!wb::split_transform(" 90"));
	/* Every name Waybox knows goes there and back. */
	for (const char *t : {"normal", "90", "180", "270", "flipped", "flipped-90",
			"flipped-180", "flipped-270"}) {
		auto p = wb::split_transform(t);
		WB_CHECK(p && wb::join_transform(p->first, p->second) == t);
	}
	WB_CHECK(wb::join_transform("", false).empty());
	WB_CHECK(wb::join_transform("", true) == "flipped");  /* a flip on its own */
}
