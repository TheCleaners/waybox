#ifndef WB_COLOR_TEXT_HPP
#define WB_COLOR_TEXT_HPP

/*
 * A color field showing its color: the field's background tinted with it
 * (partly transparent, so the field still looks like a field), and its
 * text dark or light so it stays readable on that. Pure; unit-tested.
 */
#include <optional>
#include <string>
#include <string_view>

#include "waybox/theme.hpp"

namespace wb {

struct FieldTint {
	Color background;               /* the color, at the tint's opacity */
	std::optional<Color> text;      /* black or white; nullopt: the theme's */
};

/* The tint for a field holding `value` ("#rgb", "#rrggbb", "#rrggbbaa",
 * or a name parse_color knows) on a field whose own background is `base`
 * (light or dark, as the theme is); nullopt for "" or something
 * unreadable (the field as usual). The tint's opacity follows the color's
 * own, from 20% to 70%. From 45% up, the text is black or white, whichever
 * contrasts more with the tint as it shows over `base`. */
std::optional<FieldTint> field_tint(std::string_view value,
		const Color &base = Color{255, 255, 255, 255});

/* The WCAG contrast ratio of two opaque colors (1 to 21). */
double contrast(const Color &a, const Color &b);

/* A CSS rule for entries of class `css_class` with that tint. */
std::string field_tint_css(const std::string &css_class, const FieldTint &t);

/* Relative luminance (0..1) of an sRGB color. */
double luminance(const Color &c);

}  // namespace wb

#endif
