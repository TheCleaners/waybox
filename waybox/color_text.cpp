#include "waybox/color_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wb {

namespace {

double linear(uint8_t v) {
	double c = v / 255.0;
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

double luminance(const Color &c) {
	return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

double contrast(const Color &a, const Color &b) {
	double la = luminance(a), lb = luminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

std::optional<FieldTint> field_tint(std::string_view value, const Color &base) {
	auto c = parse_color(value);
	if (!c)
		return std::nullopt;
	FieldTint t;
	double opacity = 0.2 + 0.5 * (c->a / 255.0);
	t.background = {c->r, c->g, c->b, static_cast<uint8_t>(std::lround(opacity * 255))};
	if (opacity >= 0.45) {
		/* What shows: the tint over the field's own background. */
		auto mix = [opacity](uint8_t fg, uint8_t bg) {
			return static_cast<uint8_t>(std::lround(fg * opacity + bg * (1 - opacity)));
		};
		Color shown{mix(c->r, base.r), mix(c->g, base.g), mix(c->b, base.b), 255};
		Color black{0, 0, 0, 255}, white{255, 255, 255, 255};
		t.text = contrast(black, shown) >= contrast(white, shown) ? black : white;
	}
	return t;
}

std::string field_tint_css(const std::string &css_class, const FieldTint &t) {
	char buf[256];
	const Color &b = t.background;
	int n = std::snprintf(buf, sizeof buf, "entry.%s { background-color: rgba(%d, %d, %d, %.3f);",
			css_class.c_str(), b.r, b.g, b.b, b.a / 255.0);
	std::string css(buf, static_cast<size_t>(std::max(0, n)));
	if (t.text)
		css += t.text->r == 0 ? " color: #000000;" : " color: #ffffff;";
	css += " }\n";
	return css;
}

}  // namespace wb
