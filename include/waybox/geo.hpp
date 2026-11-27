#ifndef WB_GEO_HPP
#define WB_GEO_HPP

/*
 * Places on Earth, for wbconf's location picker: country outlines and
 * cities (data/geo, from Natural Earth; tools/geo/make_geo.py), and the
 * time zone's own city (tzdata's zone1970.tab). Pure (test/geo_test.cpp).
 */
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::geo {

/* A point: degrees, latitude north and longitude east positive. */
struct Point {
	double lat = 0, lon = 0;
};

/* countries.bin's outlines (each a closed ring); none if it isn't one. */
std::vector<std::vector<Point>> load_borders(std::string_view bytes);

struct City {
	std::string name, country;
	Point at;
	long population = 0;
};
/* cities.tsv: name, country, latitude, longitude, population a line. */
std::vector<City> load_cities(std::string_view tsv);

/* The cities whose name has `query` in it, the most likely first: names
 * starting with it, then a word starting with it, then the rest, each by
 * population. Case and the usual accents don't matter ("sao paulo" finds
 * São Paulo). At most `limit`. */
std::vector<const City *> find_cities(const std::vector<City> &cities, std::string_view query,
		size_t limit);

/* The city nearest `p` (nullptr for none). */
const City *nearest_city(const std::vector<City> &cities, Point p);

/* A time zone's place, as zone1970.tab gives it ("Europe/Lisbon" ->
 * 38.72, -9.13); nullopt if it isn't there. */
std::optional<Point> zone_position(std::string_view zone1970_tab, std::string_view zone);
/* The city part of a zone's name, as read: "America/New_York" -> "New York". */
std::string zone_city(std::string_view zone);

/* A latitude or longitude as typed: a dot or a comma for the decimals
 * (either locale's way), in [min, max]; nullopt otherwise. */
std::optional<double> parse_degrees(std::string_view text, double min, double max);
/* As written for a program: a dot, two decimals ("38.72"). */
std::string format_degrees(double v);

}  // namespace wb::geo

#endif /* WB_GEO_HPP */
