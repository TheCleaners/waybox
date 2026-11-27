#include "waybox/geo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wb::geo {

std::vector<std::vector<Point>> load_borders(std::string_view b) {
	std::vector<std::vector<Point>> out;
	const std::string_view magic = "WBGEO1\n";
	if (b.substr(0, magic.size()) != magic)
		return out;
	size_t at = magic.size();
	auto u32 = [&b](size_t i) {
		return static_cast<uint32_t>(static_cast<uint8_t>(b[i])) |
				static_cast<uint32_t>(static_cast<uint8_t>(b[i + 1])) << 8 |
				static_cast<uint32_t>(static_cast<uint8_t>(b[i + 2])) << 16 |
				static_cast<uint32_t>(static_cast<uint8_t>(b[i + 3])) << 24;
	};
	auto i16 = [&b](size_t i) {
		return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint8_t>(b[i]) |
				static_cast<uint8_t>(b[i + 1]) << 8));
	};
	while (at + 4 <= b.size()) {
		uint32_t n = u32(at);
		at += 4;
		if (n > (b.size() - at) / 4)
			return {};  /* cut short: none rather than half */
		std::vector<Point> ring;
		ring.reserve(n);
		for (uint32_t k = 0; k < n; ++k, at += 4)
			ring.push_back({i16(at + 2) / 100.0, i16(at) / 100.0});
		out.push_back(std::move(ring));
	}
	return out;
}

std::vector<City> load_cities(std::string_view tsv) {
	std::vector<City> out;
	size_t at = 0;
	while (at < tsv.size()) {
		size_t nl = tsv.find('\n', at);
		std::string_view line = tsv.substr(at, nl == std::string_view::npos ? std::string_view::npos
				: nl - at);
		at = nl == std::string_view::npos ? tsv.size() : nl + 1;
		std::vector<std::string_view> f;
		size_t s = 0;
		while (true) {
			size_t tab = line.find('\t', s);
			f.push_back(line.substr(s, tab == std::string_view::npos ? std::string_view::npos
					: tab - s));
			if (tab == std::string_view::npos)
				break;
			s = tab + 1;
		}
		if (f.size() < 4 || f[0].empty())
			continue;
		City c;
		c.name = std::string(f[0]);
		c.country = std::string(f[1]);
		c.at.lat = std::strtod(std::string(f[2]).c_str(), nullptr);
		c.at.lon = std::strtod(std::string(f[3]).c_str(), nullptr);
		if (f.size() > 4)
			c.population = std::strtol(std::string(f[4]).c_str(), nullptr, 10);
		out.push_back(std::move(c));
	}
	return out;
}

namespace {

/* Lowercase, the usual Latin accents taken off ("São" -> "sao"). */
std::string fold(std::string_view s) {
	static const std::pair<const char *, char> accents[] = {
		{"á", 'a'}, {"à", 'a'}, {"â", 'a'}, {"ä", 'a'}, {"ã", 'a'}, {"å", 'a'}, {"Á", 'a'},
		{"Å", 'a'}, {"Ä", 'a'}, {"é", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'}, {"É", 'e'},
		{"í", 'i'}, {"ì", 'i'}, {"î", 'i'}, {"ï", 'i'}, {"Í", 'i'}, {"İ", 'i'}, {"ı", 'i'},
		{"ó", 'o'}, {"ò", 'o'}, {"ô", 'o'}, {"ö", 'o'}, {"õ", 'o'}, {"ø", 'o'}, {"Ö", 'o'},
		{"Ø", 'o'}, {"ú", 'u'}, {"ù", 'u'}, {"û", 'u'}, {"ü", 'u'}, {"Ü", 'u'}, {"ç", 'c'},
		{"Ç", 'c'}, {"ñ", 'n'}, {"ń", 'n'}, {"ł", 'l'}, {"Ł", 'l'}, {"ś", 's'}, {"š", 's'},
		{"Š", 's'}, {"ş", 's'}, {"ș", 's'}, {"ž", 'z'}, {"ź", 'z'}, {"ż", 'z'}, {"Ž", 'z'},
		{"č", 'c'}, {"ć", 'c'}, {"Č", 'c'}, {"ř", 'r'}, {"ě", 'e'}, {"ą", 'a'}, {"ę", 'e'},
		{"ğ", 'g'}, {"ý", 'y'}, {"ţ", 't'}, {"ț", 't'}, {"đ", 'd'}, {"ő", 'o'}, {"ű", 'u'},
	};
	std::string out;
	for (size_t i = 0; i < s.size();) {
		bool done = false;
		if (static_cast<unsigned char>(s[i]) >= 0x80)
			for (const auto &[from, to] : accents) {
				size_t n = strlen(from);
				if (s.compare(i, n, from) == 0) {
					out += to;
					i += n;
					done = true;
					break;
				}
			}
		if (done)
			continue;
		char c = s[i++];
		out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c == '-' ? ' ' : c;
	}
	return out;
}

}  // namespace

std::vector<const City *> find_cities(const std::vector<City> &cities, std::string_view query,
		size_t limit) {
	std::string q = fold(query);
	while (!q.empty() && q.back() == ' ')
		q.pop_back();
	while (!q.empty() && q.front() == ' ')
		q.erase(0, 1);
	std::vector<const City *> starts, word, inside;
	if (q.empty())
		return {};
	for (const City &c : cities) {
		std::string n = fold(c.name);
		size_t at = n.find(q);
		if (at == std::string::npos)
			continue;
		if (at == 0)
			starts.push_back(&c);
		else if (n[at - 1] == ' ' || n[at - 1] == '\'')
			word.push_back(&c);
		else
			inside.push_back(&c);
	}
	auto by_population = [](const City *a, const City *b) { return a->population > b->population; };
	std::vector<const City *> out;
	for (auto *group : {&starts, &word, &inside}) {
		std::stable_sort(group->begin(), group->end(), by_population);
		for (const City *c : *group) {
			if (out.size() >= limit)
				return out;
			out.push_back(c);
		}
	}
	return out;
}

const City *nearest_city(const std::vector<City> &cities, Point p) {
	const City *best = nullptr;
	double best_d = 0;
	for (const City &c : cities) {
		/* Near enough the sphere's distance for choosing the nearest. */
		double dlat = c.at.lat - p.lat;
		double dlon = std::remainder(c.at.lon - p.lon, 360.0) *
				std::cos((c.at.lat + p.lat) / 2 * M_PI / 180);
		double d = dlat * dlat + dlon * dlon;
		if (best == nullptr || d < best_d) {
			best = &c;
			best_d = d;
		}
	}
	return best;
}

namespace {

/* "+3843" / "-00908" / "+384300" -> degrees (ISO 6709, as zone1970.tab
 * has them: DDMM or DDMMSS, DDDMM or DDDMMSS for longitudes). */
std::optional<double> iso6709(std::string_view s, int degree_digits) {
	if (s.size() < static_cast<size_t>(1 + degree_digits + 2) || (s[0] != '+' && s[0] != '-'))
		return std::nullopt;
	for (char c : s.substr(1))
		if (c < '0' || c > '9')
			return std::nullopt;
	auto num = [&s](size_t from, size_t n) { return std::stoi(std::string(s.substr(from, n))); };
	double v = num(1, static_cast<size_t>(degree_digits)) +
			num(1 + static_cast<size_t>(degree_digits), 2) / 60.0;
	if (s.size() >= static_cast<size_t>(1 + degree_digits + 4))
		v += num(1 + static_cast<size_t>(degree_digits) + 2, 2) / 3600.0;
	return s[0] == '-' ? -v : v;
}

}  // namespace

std::optional<Point> zone_position(std::string_view tab, std::string_view zone) {
	size_t at = 0;
	while (at < tab.size()) {
		size_t nl = tab.find('\n', at);
		std::string_view line = tab.substr(at, nl == std::string_view::npos ? std::string_view::npos
				: nl - at);
		at = nl == std::string_view::npos ? tab.size() : nl + 1;
		if (line.empty() || line[0] == '#')
			continue;
		size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
		if (t1 == std::string_view::npos || t2 == std::string_view::npos)
			continue;
		size_t t3 = line.find('\t', t2 + 1);
		std::string_view coords = line.substr(t1 + 1, t2 - t1 - 1);
		std::string_view name = line.substr(t2 + 1, t3 == std::string_view::npos
				? std::string_view::npos : t3 - t2 - 1);
		if (name != zone)
			continue;
		size_t split = coords.find_first_of("+-", 1);
		if (split == std::string_view::npos)
			return std::nullopt;
		auto lat = iso6709(coords.substr(0, split), 2);
		auto lon = iso6709(coords.substr(split), 3);
		if (!lat || !lon)
			return std::nullopt;
		return Point{*lat, *lon};
	}
	return std::nullopt;
}

std::string zone_city(std::string_view zone) {
	size_t slash = zone.rfind('/');
	std::string city(slash == std::string_view::npos ? zone : zone.substr(slash + 1));
	std::replace(city.begin(), city.end(), '_', ' ');
	return city;
}

std::optional<double> parse_degrees(std::string_view text, double min, double max) {
	std::string t(text);
	while (!t.empty() && t.back() == ' ')
		t.pop_back();
	while (!t.empty() && t.front() == ' ')
		t.erase(0, 1);
	if (t.empty() || t.size() > 16)
		return std::nullopt;
	std::replace(t.begin(), t.end(), ',', '.');
	size_t i = t[0] == '-' || t[0] == '+' ? 1 : 0;
	if (i >= t.size() || t.find_first_not_of("0123456789.", i) != std::string::npos ||
			std::count(t.begin(), t.end(), '.') > 1 || t.find_first_of("0123456789") ==
			std::string::npos)
		return std::nullopt;
	/* Not strtod: that reads the locale's decimal separator. */
	double whole = 0, frac = 0, scale = 1;
	bool after = false;
	for (size_t k = i; k < t.size(); ++k) {
		if (t[k] == '.') {
			after = true;
			continue;
		}
		if (after) {
			scale /= 10;
			frac += (t[k] - '0') * scale;
		} else {
			whole = whole * 10 + (t[k] - '0');
		}
	}
	double v = (whole + frac) * (t[0] == '-' ? -1 : 1);
	if (v < min || v > max)
		return std::nullopt;
	return v;
}

std::string format_degrees(double v) {
	/* Two decimals, always a dot (not the locale's separator). */
	long hundredths = std::lround(v * 100);
	char buf[32];
	snprintf(buf, sizeof buf, "%s%ld.%02ld", hundredths < 0 ? "-" : "",
			std::labs(hundredths) / 100, std::labs(hundredths) % 100);
	return buf;
}

}  // namespace wb::geo
