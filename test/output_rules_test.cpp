#include "wb_test.hpp"

#include "waybox/output_rules.hpp"

using namespace wb;

WB_TEST(parse_mode_forms) {
	int w = 0, h = 0;
	double hz = -1;
	WB_CHECK(parse_mode("3840x2160@60", w, h, hz) && w == 3840 && h == 2160 && hz == 60);
	WB_CHECK(parse_mode("1920x1080@59.94Hz", w, h, hz) && hz > 59.9 && hz < 60);
	WB_CHECK(parse_mode(" 2560x1440 ", w, h, hz) && w == 2560 && h == 1440 && hz == 0);
	for (const char *bad : {"", "1920", "1920x", "x1080", "1920x1080@", "1920x1080@0",
			"axb", "1920x1080@sixty", "-1x5"})
		WB_CHECK(!parse_mode(bad, w, h, hz));
}

WB_TEST(parse_position_and_transform) {
	int x = 0, y = 0;
	WB_CHECK(parse_position("1920,0", x, y) && x == 1920 && y == 0);
	WB_CHECK(parse_position(" -1280 , 200 ", x, y) && x == -1280 && y == 200);
	WB_CHECK(!parse_position("1920", x, y));
	WB_CHECK(!parse_position("a,b", x, y));
	WB_CHECK(parse_transform("normal") == 0);
	WB_CHECK(parse_transform("270") == 3);
	WB_CHECK(parse_transform("flipped-90") == 5);
	WB_CHECK(!parse_transform("sideways"));
}

WB_TEST(match_by_name_or_identity) {
	std::vector<OutputRule> rules(3);
	rules[0].match.make = "Dell Inc.";
	rules[0].match.serial = "ABC123";
	rules[1].match.name = "HDMI-A-1";
	/* rules[2] has no match fields: it matches nothing. */
	OutputIdentity dell{"DP-3", "Dell Inc.", "U2723QE", "ABC123"};
	OutputIdentity other_dell{"DP-3", "Dell Inc.", "U2723QE", "XYZ"};
	OutputIdentity tv{"HDMI-A-1", "Samsung", "TV", ""};
	OutputIdentity unknown{"DP-1", "", "", ""};
	WB_CHECK(match_output(rules, dell) == &rules[0]);
	WB_CHECK(match_output(rules, other_dell) == nullptr);
	WB_CHECK(match_output(rules, tv) == &rules[1]);
	WB_CHECK(match_output(rules, unknown) == nullptr);
}

/* A TV that marks 4K@30 preferred but also does 4K@60. */
static const std::vector<ModeInfo> kTv = {
	{3840, 2160, 30000, true}, {3840, 2160, 60000, false}, {3840, 2160, 59940, false},
	{1920, 1080, 60000, false}, {1920, 1080, 120000, false},
};

WB_TEST(pick_mode_by_refresh) {
	WB_CHECK(pick_mode(kTv, 3840, 2160, 60) == 1);
	WB_CHECK(pick_mode(kTv, 3840, 2160, 59.94) == 2);
	WB_CHECK(pick_mode(kTv, 3840, 2160, 30) == 0);
	WB_CHECK(pick_mode(kTv, 3840, 2160, 0) == 1);   /* the highest */
	WB_CHECK(pick_mode(kTv, 1920, 1080, 144) == 4); /* the closest */
	WB_CHECK(pick_mode(kTv, 1280, 720, 60) == -1);
}

WB_TEST(default_mode_prefers_the_highest_refresh_at_the_preferred_size) {
	WB_CHECK(default_mode(kTv) == 1);
	std::vector<ModeInfo> no_preferred = {{1280, 1024, 75000, false},
			{1920, 1080, 50000, false}, {1920, 1080, 60000, false}};
	WB_CHECK(default_mode(no_preferred) == 2);
	WB_CHECK(default_mode({}) == -1);
}
