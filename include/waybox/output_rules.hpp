#ifndef WB_OUTPUT_RULES_HPP
#define WB_OUTPUT_RULES_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * Output settings from rc.xml (<waybox><outputs><output .../></outputs>),
 * kept free of wlroots so they can be unit-tested.
 */

/* What a rule matches: every field the rule sets must be equal. A rule with
 * no match fields matches nothing. */
struct OutputIdentity {
	std::string name;    /* connector, e.g. HDMI-A-1 */
	std::string make;
	std::string model;
	std::string serial;
};

struct OutputRule {
	OutputIdentity match;
	std::optional<bool> enabled;
	/* The mode; width 0 = not set. refresh 0 = the highest at that size. */
	int width = 0;
	int height = 0;
	double refresh = 0;
	std::optional<double> scale;
	std::optional<int> x, y;
	std::optional<int> transform;  /* enum wl_output_transform */
	std::optional<bool> adaptive_sync;
};

struct ModeInfo {
	int width;
	int height;
	int refresh_mhz;
	bool preferred;
};

/* "3840x2160" or "3840x2160@60" (Hz, decimals allowed, an optional "Hz"). */
bool parse_mode(std::string_view text, int &width, int &height, double &refresh);
/* "X,Y". */
bool parse_position(std::string_view text, int &x, int &y);
/* normal, 90, 180, 270, flipped, flipped-90, flipped-180, flipped-270. */
std::optional<int> parse_transform(std::string_view text);

/* The first rule that matches `output`, or null. */
const OutputRule *match_output(const std::vector<OutputRule> &rules,
		const OutputIdentity &output);

/* The mode at width x height closest to `refresh` Hz (or the highest, when
 * refresh is 0). -1 when the output has no mode of that size. */
int pick_mode(const std::vector<ModeInfo> &modes, int width, int height,
		double refresh);

/* The mode to use without a rule: the preferred resolution at its highest
 * refresh rate (TVs often mark a 30 Hz mode preferred, though they do 60 Hz).
 * Without a preferred mode, the largest. -1 when there are no modes. */
int default_mode(const std::vector<ModeInfo> &modes);

}  // namespace wb

#endif /* WB_OUTPUT_RULES_HPP */
