#include "waybox/output_rules.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace wb {

namespace {

bool to_int(std::string_view s, int &out) {
	if (s.empty())
		return false;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
	return ec == std::errc() && p == s.data() + s.size();
}

std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

}  // namespace

bool parse_mode(std::string_view text, int &width, int &height, double &refresh) {
	text = trim(text);
	double hz = 0;
	if (size_t at = text.find('@'); at != std::string_view::npos) {
		std::string rate(text.substr(at + 1));
		if (rate.size() > 2 && (rate.ends_with("Hz") || rate.ends_with("hz")))
			rate.resize(rate.size() - 2);
		char *end = nullptr;
		hz = std::strtod(rate.c_str(), &end);
		if (rate.empty() || end != rate.c_str() + rate.size() || !(hz > 0))
			return false;
		text = text.substr(0, at);
	}
	size_t x = text.find('x');
	int w, h;
	if (x == std::string_view::npos || !to_int(text.substr(0, x), w) ||
			!to_int(text.substr(x + 1), h) || w <= 0 || h <= 0)
		return false;
	width = w;
	height = h;
	refresh = hz;
	return true;
}

bool parse_position(std::string_view text, int &x, int &y) {
	text = trim(text);
	size_t comma = text.find(',');
	int a, b;
	if (comma == std::string_view::npos || !to_int(trim(text.substr(0, comma)), a) ||
			!to_int(trim(text.substr(comma + 1)), b))
		return false;
	x = a;
	y = b;
	return true;
}

std::optional<int> parse_transform(std::string_view text) {
	/* The order of enum wl_output_transform. */
	static constexpr std::string_view names[] = {"normal", "90", "180", "270",
			"flipped", "flipped-90", "flipped-180", "flipped-270"};
	text = trim(text);
	for (int i = 0; i < 8; i++)
		if (text == names[i])
			return i;
	return std::nullopt;
}

const OutputRule *match_output(const std::vector<OutputRule> &rules,
		const OutputIdentity &output) {
	for (const OutputRule &r : rules) {
		const OutputIdentity &m = r.match;
		if (m.name.empty() && m.make.empty() && m.model.empty() && m.serial.empty())
			continue;
		if ((m.name.empty() || m.name == output.name) &&
				(m.make.empty() || m.make == output.make) &&
				(m.model.empty() || m.model == output.model) &&
				(m.serial.empty() || m.serial == output.serial))
			return &r;
	}
	return nullptr;
}

int pick_mode(const std::vector<ModeInfo> &modes, int width, int height,
		double refresh) {
	int best = -1;
	for (int i = 0; i < (int) modes.size(); i++) {
		const ModeInfo &m = modes[i];
		if (m.width != width || m.height != height)
			continue;
		if (best < 0) {
			best = i;
			continue;
		}
		int b = modes[best].refresh_mhz;
		if (refresh > 0) {
			double want = refresh * 1000.0;
			if (std::fabs(m.refresh_mhz - want) < std::fabs(b - want))
				best = i;
		} else if (m.refresh_mhz > b) {
			best = i;
		}
	}
	return best;
}

int default_mode(const std::vector<ModeInfo> &modes) {
	for (const ModeInfo &m : modes)
		if (m.preferred)
			return pick_mode(modes, m.width, m.height, 0);
	int best = -1;
	for (int i = 0; i < (int) modes.size(); i++) {
		const ModeInfo &m = modes[i];
		if (best < 0)
			best = i;
		long area = (long) m.width * m.height;
		long barea = (long) modes[best].width * modes[best].height;
		if (area > barea || (area == barea && m.refresh_mhz > modes[best].refresh_mhz))
			best = i;
	}
	return best;
}

}  // namespace wb
