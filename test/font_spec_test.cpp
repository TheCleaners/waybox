#include "wb_test.hpp"

#include "waybox/font_spec.hpp"

using wb::font::Spec;

WB_TEST(font_fontconfig_reads) {
	auto s = wb::font::from_fontconfig("Iosevka Term, Noto Color Emoji:size=12:weight=bold:"
			"slant=italic:width=condensed:antialias=true:fontfeatures=calt=0");
	WB_CHECK((s.families == std::vector<std::string>{"Iosevka Term", "Noto Color Emoji"}));
	WB_CHECK(s.size == 12 && s.weight == 700 && s.slant == "italic" && s.width == 75);
	WB_CHECK((s.other == std::vector<std::string>{"antialias=true", "fontfeatures=calt=0"}));
	/* Bare words, other spellings, fontconfig's numeric weights. */
	auto b = wb::font::from_fontconfig("monospace:bold:italic:size=10.5");
	WB_CHECK(b.weight == 700 && b.slant == "italic" && b.size == 10.5);
	WB_CHECK(wb::font::from_fontconfig("x:weight=demibold").weight == 600);
	WB_CHECK(wb::font::from_fontconfig("x:weight=book").weight == 400);
	WB_CHECK(wb::font::from_fontconfig("x:weight=200").weight == 700);  /* FC_WEIGHT_BOLD */
	WB_CHECK(wb::font::from_fontconfig("x:weight=80").weight == 400);
	WB_CHECK(wb::font::from_fontconfig("x:slant=roman").slant.empty());
	/* Nothing. */
	auto e = wb::font::from_fontconfig("");
	WB_CHECK(e.families.empty() && e.size == 0 && e.other.empty());
}

WB_TEST(font_fontconfig_writes) {
	const char *t = "Iosevka,Noto Color Emoji:size=12:weight=bold:width=condensed:antialias=true";
	WB_CHECK(wb::font::to_fontconfig(wb::font::from_fontconfig(t)) == t);
	Spec s = wb::font::from_fontconfig("monospace:size=11:fontfeatures=liga=0");
	s.families[0] = "JetBrains Mono";
	s.weight = 450;  /* not offered: the nearest */
	s.slant = "italic";
	WB_CHECK(wb::font::to_fontconfig(s) ==
			"JetBrains Mono:size=11:weight=regular:slant=italic:fontfeatures=liga=0");
	s.size = 0;
	s.weight = 0;
	s.slant.clear();
	WB_CHECK(wb::font::to_fontconfig(s) == "JetBrains Mono:fontfeatures=liga=0");
}

WB_TEST(font_nearest) {
	WB_CHECK(wb::font::nearest_weight(450) == 400 || wb::font::nearest_weight(450) == 500);
	WB_CHECK(wb::font::nearest_weight(650) == 600 || wb::font::nearest_weight(650) == 700);
	WB_CHECK(wb::font::nearest_weight(1000) == 900);
	WB_CHECK(wb::font::nearest_width(80) == 75 && wb::font::nearest_width(300) == 200);
}

WB_TEST(font_pango_words) {
	using wb::font::Styled;
	auto s = wb::font::from_pango("Inter Semi-Bold Italic Condensed 11");
	WB_CHECK(s.family == "Inter" && s.size == 11 && s.weight == 600 && s.italic &&
			s.width == 75);
	WB_CHECK(wb::font::to_pango(s) == "Inter Semi-Bold Italic Condensed 11");
	/* Numbers in the family; no style. */
	auto n = wb::font::from_pango("3270 Nerd Font 11");
	WB_CHECK(n.family == "3270 Nerd Font" && n.size == 11 && n.weight == 400 && !n.italic);
	WB_CHECK(wb::font::to_pango(n) == "3270 Nerd Font 11");
	/* Spellings: as Pango, fontconfig or CSS write them. */
	WB_CHECK(wb::font::from_pango("Sans Bold 10").weight == 700);
	WB_CHECK(wb::font::from_pango("Sans semibold 10").weight == 600);
	WB_CHECK(wb::font::from_pango("Sans Ultra-Light 10").weight == 200);
	WB_CHECK(wb::font::from_pango("Sans Oblique 10").italic);
	WB_CHECK(wb::font::from_pango("Sans Semi-Expanded 10").width == 113);
	/* A family that is a style word alone stays the family. */
	WB_CHECK(wb::font::from_pango("Light 10").family == "Light");
	/* Fractions, and a normal style left out. */
	WB_CHECK(wb::font::to_pango({"Inter", 11.5, 400, false, 100}) == "Inter 11.5");
	WB_CHECK(wb::font::to_pango({"Inter", 0, 900, false, 100}) == "Inter Heavy");
	/* rc.xml's names. */
	WB_CHECK(wb::font::weight_named("bold") == 700 && wb::font::weight_named("normal") == 400 &&
			wb::font::weight_named("Semi-Bold") == 600 && wb::font::weight_named("650") == 650 &&
			wb::font::weight_named("fat") == 0 && wb::font::weight_named("") == 0);
	WB_CHECK(wb::font::width_named("condensed") == 75 &&
			wb::font::width_named("Semi-Expanded") == 113 && wb::font::width_named("90") == 90 &&
			wb::font::width_named("wide") == 0);
}

