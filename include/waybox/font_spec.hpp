#ifndef WB_FONT_SPEC_HPP
#define WB_FONT_SPEC_HPP

/*
 * A font as a page's font control edits it: families (the first chosen,
 * the rest fallbacks), size, weight, slant and width; read from and
 * written as fontconfig names ("Iosevka,Noto Color Emoji:size=12:
 * weight=bold:width=condensed", as foot and fuzzel take them), keeping
 * the properties it doesn't edit (antialias=..., fontfeatures=...) as
 * they were. Weights on the OpenType scale (100..900), widths in percent
 * (100 normal). Pure; unit-tested.
 */
#include <string>
#include <string_view>
#include <vector>

namespace wb::font {

struct Spec {
	std::vector<std::string> families;
	double size = 0;   /* points; 0: not given */
	int weight = 0;    /* 100..1000; 0: not given */
	std::string slant; /* "", "italic" or "oblique" */
	int width = 0;     /* percent; 0: not given */
	std::vector<std::string> other;  /* "key=value" kept as written */
	bool operator==(const Spec &) const = default;
};

Spec from_fontconfig(std::string_view text);
std::string to_fontconfig(const Spec &spec);

/* The names wbconf shows and fontconfig writes, for the weights and widths
 * its menus offer: {100, "thin", "Thin"}... */
struct Named {
	int value;
	const char *fc;      /* fontconfig's name */
	const char *label;   /* as shown */
};
const std::vector<Named> &weights();
const std::vector<Named> &widths();
/* The nearest one offered (a weight of 450: Regular). */
int nearest_weight(int weight);
int nearest_width(int width);

/* A weight or width named in any of the usual spellings: fontconfig's
 * ("semibold"), Pango's ("Semi-Bold"), CSS's ("normal", "bold"), other
 * ones ("demibold", "heavy", "book"), or a number. 0: not one. */
int weight_named(std::string_view name);
int width_named(std::string_view name);

/* A font as Pango writes it, as wbconf's own font rows hold one: "Inter
 * Semi-Bold Italic Condensed 11". The family, then the style words, then
 * the size (whole or not); a normal weight, slant and width not written. */
struct Styled {
	std::string family;
	double size = 0;
	int weight = 400;
	bool italic = false;
	int width = 100;
	bool operator==(const Styled &) const = default;
};
Styled from_pango(std::string_view text);
std::string to_pango(const Styled &s);

}  // namespace wb::font

#endif
