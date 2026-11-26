#include "waybox/font_spec.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wb::font {

namespace {

std::string_view trim(std::string_view s) {
	size_t b = s.find_first_not_of(" \t");
	if (b == std::string_view::npos)
		return {};
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
	std::vector<std::string_view> out;
	size_t start = 0;
	for (;;) {
		size_t at = s.find(sep, start);
		out.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos
				: at - start));
		if (at == std::string_view::npos)
			return out;
		start = at + 1;
	}
}

/* fontconfig's own scale for a numeric weight (FC_WEIGHT_*), to
 * OpenType's: the points it defines, in between linearly. */
int fc_numeric_weight(double fc) {
	static const double fcv[] = {0, 40, 50, 55, 75, 80, 100, 180, 200, 205, 210, 215};
	static const double ot[] = {100, 200, 300, 350, 380, 400, 500, 600, 700, 800, 900, 1000};
	if (fc <= 0)
		return 100;
	for (size_t i = 1; i < sizeof fcv / sizeof fcv[0]; ++i)
		if (fc <= fcv[i])
			return static_cast<int>(std::lround(ot[i - 1] + (ot[i] - ot[i - 1]) *
					(fc - fcv[i - 1]) / (fcv[i] - fcv[i - 1])));
	return 1000;
}

std::string number(double v) {
	char buf[32];
	std::snprintf(buf, sizeof buf, "%g", v);
	return buf;
}

}  // namespace

const std::vector<Named> &weights() {
	static const std::vector<Named> w = {
		{100, "thin", "Thin"}, {200, "extralight", "Extra Light"}, {300, "light", "Light"},
		{400, "regular", "Regular"}, {500, "medium", "Medium"}, {600, "semibold", "Semibold"},
		{700, "bold", "Bold"}, {800, "extrabold", "Extra Bold"}, {900, "black", "Black"},
	};
	return w;
}

const std::vector<Named> &widths() {
	static const std::vector<Named> w = {
		{50, "ultracondensed", "Ultra Condensed"}, {63, "extracondensed", "Extra Condensed"},
		{75, "condensed", "Condensed"}, {87, "semicondensed", "Semi Condensed"},
		{100, "normal", "Normal"}, {113, "semiexpanded", "Semi Expanded"},
		{125, "expanded", "Expanded"}, {150, "extraexpanded", "Extra Expanded"},
		{200, "ultraexpanded", "Ultra Expanded"},
	};
	return w;
}

int nearest_weight(int weight) {
	int best = 400;
	for (const Named &n : weights())
		if (std::abs(n.value - weight) < std::abs(best - weight))
			best = n.value;
	return best;
}

int nearest_width(int width) {
	int best = 100;
	for (const Named &n : widths())
		if (std::abs(n.value - width) < std::abs(best - width))
			best = n.value;
	return best;
}

namespace {

std::string lower(std::string_view s) {
	std::string out(s);
	for (char &c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

/* Without '-', '_' and ' ': "Semi-Bold", "semi bold", "semibold" alike. */
std::string squashed(std::string_view s) {
	std::string out;
	for (char c : lower(s))
		if (c != '-' && c != '_' && c != ' ')
			out += c;
	return out;
}

/* How Pango writes each weight and width. */
const char *pango_weight(int w) {
	switch (nearest_weight(w)) {
	case 100: return "Thin";
	case 200: return "Ultra-Light";
	case 300: return "Light";
	case 500: return "Medium";
	case 600: return "Semi-Bold";
	case 700: return "Bold";
	case 800: return "Ultra-Bold";
	case 900: return "Heavy";
	default: return "";
	}
}

const char *pango_width(int w) {
	switch (nearest_width(w)) {
	case 50: return "Ultra-Condensed";
	case 63: return "Extra-Condensed";
	case 75: return "Condensed";
	case 87: return "Semi-Condensed";
	case 113: return "Semi-Expanded";
	case 125: return "Expanded";
	case 150: return "Extra-Expanded";
	case 200: return "Ultra-Expanded";
	default: return "";
	}
}

}  // namespace

int weight_named(std::string_view name) {
	std::string n = squashed(name);
	if (n.empty())
		return 0;
	static const std::pair<const char *, int> names[] = {
		{"thin", 100}, {"hairline", 100}, {"ultralight", 200}, {"extralight", 200},
		{"light", 300}, {"semilight", 350}, {"book", 380}, {"regular", 400},
		{"normal", 400}, {"medium", 500}, {"semibold", 600}, {"demibold", 600},
		{"bold", 700}, {"ultrabold", 800}, {"extrabold", 800}, {"heavy", 900},
		{"black", 900}, {"ultraheavy", 1000}, {"extrablack", 1000},
	};
	for (const auto &[k, v] : names)
		if (n == k)
			return v;
	char *end = nullptr;
	long v = std::strtol(n.c_str(), &end, 10);
	return end && *end == '\0' && v >= 1 && v <= 1000 ? static_cast<int>(v) : 0;
}

int width_named(std::string_view name) {
	std::string n = squashed(name);
	if (n.empty())
		return 0;
	for (const Named &w : widths())
		if (n == w.fc)
			return w.value;
	char *end = nullptr;
	long v = std::strtol(n.c_str(), &end, 10);
	return end && *end == '\0' && v >= 25 && v <= 400 ? static_cast<int>(v) : 0;
}

Styled from_pango(std::string_view text) {
	std::vector<std::string> words;
	std::string cur;
	for (char c : text) {
		if (c == ' ' || c == '\t') {
			if (!cur.empty())
				words.push_back(std::move(cur)), cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty())
		words.push_back(cur);
	Styled s;
	/* The size: a number last ("3270 Nerd Font 11": only the last one). */
	if (!words.empty()) {
		char *end = nullptr;
		double v = std::strtod(words.back().c_str(), &end);
		if (end && *end == '\0' && v > 0) {
			s.size = v;
			words.pop_back();
		}
	}
	/* Style words before it, from the end; the rest is the family. */
	while (words.size() > 1) {
		std::string n = squashed(words.back());
		if (n == "italic" || n == "oblique") {
			s.italic = true;
		} else if (int w = width_named(n); w > 0 && n.find_first_of("0123456789") ==
				std::string::npos) {
			s.width = w;
		} else if (int wt = weight_named(n); wt > 0 && n.find_first_of("0123456789") ==
				std::string::npos) {
			s.weight = wt;
		} else if (n == "smallcaps" || n == "notrotated") {
			/* drawn as is */
		} else {
			break;
		}
		words.pop_back();
	}
	for (size_t i = 0; i < words.size(); ++i)
		s.family += (i ? " " : "") + words[i];
	if (!s.family.empty() && s.family.back() == ',')
		s.family.pop_back();
	return s;
}

std::string to_pango(const Styled &s) {
	std::string out = s.family;
	auto word = [&out](const char *w) {
		if (w && *w)
			out += std::string(out.empty() ? "" : " ") + w;
	};
	if (s.weight != 400)
		word(pango_weight(s.weight));
	if (s.italic)
		word("Italic");
	if (s.width != 100)
		word(pango_width(s.width));
	if (s.size > 0)
		out += (out.empty() ? "" : " ") + number(s.size);
	return out;
}

Spec from_fontconfig(std::string_view text) {
	Spec s;
	auto parts = split(text, ':');
	for (std::string_view f : split(parts[0], ','))
		if (std::string_view t = trim(f); !t.empty())
			s.families.emplace_back(t);
	for (size_t i = 1; i < parts.size(); ++i) {
		std::string_view p = trim(parts[i]);
		if (p.empty())
			continue;
		size_t eq = p.find('=');
		std::string key(trim(p.substr(0, eq)));
		std::string value(eq == std::string_view::npos ? "" : trim(p.substr(eq + 1)));
		char *end = nullptr;
		double num = std::strtod(value.c_str(), &end);
		bool numeric = !value.empty() && end && *end == '\0';
		bool taken = false;
		if (key == "size" && numeric) {
			s.size = num;
			taken = true;
		} else if (key == "weight") {
			for (const Named &n : weights())
				if (value == n.fc)
					s.weight = n.value, taken = true;
			/* fontconfig's other spellings */
			static const Named also[] = {{400, "book", ""}, {400, "normal", ""},
					{600, "demibold", ""}, {200, "ultralight", ""}, {800, "ultrabold", ""},
					{900, "heavy", ""}};
			for (const Named &n : also)
				if (value == n.fc)
					s.weight = n.value, taken = true;
			if (!taken && numeric)
				s.weight = fc_numeric_weight(num), taken = true;
		} else if (key == "slant") {
			if (value == "italic" || value == "oblique")
				s.slant = value, taken = true;
			else if (value == "roman")
				taken = true;
		} else if (key == "width") {
			for (const Named &n : widths())
				if (value == n.fc)
					s.width = n.value, taken = true;
			if (!taken && numeric)
				s.width = static_cast<int>(num), taken = true;
		}
		/* Bare words (":bold", ":italic") as fontconfig takes them. */
		if (!taken && eq == std::string_view::npos) {
			for (const Named &n : weights())
				if (key == n.fc)
					s.weight = n.value, taken = true;
			if (key == "italic" || key == "oblique")
				s.slant = key, taken = true;
			for (const Named &n : widths())
				if (key == n.fc)
					s.width = n.value, taken = true;
		}
		if (!taken)
			s.other.emplace_back(p);
	}
	return s;
}

std::string to_fontconfig(const Spec &s) {
	std::string out;
	for (size_t i = 0; i < s.families.size(); ++i)
		out += (i ? "," : "") + s.families[i];
	if (s.size > 0)
		out += ":size=" + number(s.size);
	if (s.weight > 0) {
		std::string name;
		for (const Named &n : weights())
			if (n.value == s.weight)
				name = n.fc;
		if (name.empty())
			for (const Named &n : weights())
				if (n.value == nearest_weight(s.weight))
					name = n.fc;
		out += ":weight=" + name;
	}
	if (!s.slant.empty())
		out += ":slant=" + s.slant;
	if (s.width > 0) {
		std::string name;
		for (const Named &n : widths())
			if (n.value == s.width)
				name = n.fc;
		out += ":width=" + (name.empty() ? std::to_string(s.width) : name);
	}
	for (const std::string &o : s.other)
		out += ":" + o;
	return out;
}

}  // namespace wb::font
