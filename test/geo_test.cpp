#include "wb_test.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

#include "waybox/geo.hpp"

using namespace wb::geo;

namespace {

bool near(double a, double b) {
	return std::fabs(a - b) < 0.01;
}

std::string slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

const char *kCities =
	"Tokyo\tJapan\t35.69\t139.75\t35676000\n"
	"São Paulo\tBrazil\t-23.56\t-46.63\t18845000\n"
	"Lisbon\tPortugal\t38.72\t-9.14\t2812000\n"
	"New York\tUnited States of America\t40.72\t-74.00\t19040000\n"
	"Newcastle\tUnited Kingdom\t54.98\t-1.62\t882000\n"
	"Kraków\tPoland\t50.06\t19.96\t756000\n";

}  // namespace

WB_TEST(geo_cities_found) {
	auto cities = load_cities(kCities);
	WB_CHECK(cities.size() == 6);
	WB_CHECK(cities[1].name == "São Paulo" && near(cities[1].at.lat, -23.56));
	/* Starting with it first, bigger first; accents and case don't matter. */
	auto found = find_cities(cities, "new", 5);
	WB_CHECK(found.size() == 2 && found[0]->name == "New York" && found[1]->name == "Newcastle");
	found = find_cities(cities, "sao paulo", 5);
	WB_CHECK(found.size() == 1 && found[0]->name == "São Paulo");
	found = find_cities(cities, "KRAKOW", 5);
	WB_CHECK(found.size() == 1 && found[0]->name == "Kraków");
	/* A word starting with it before the middle of one. */
	found = find_cities(cities, "york", 5);
	WB_CHECK(found.size() == 1 && found[0]->name == "New York");
	found = find_cities(cities, "o", 2);
	WB_CHECK(found.size() == 2);
	WB_CHECK(find_cities(cities, "   ", 5).empty());
	/* The nearest one, across the date line too. */
	WB_CHECK(nearest_city(cities, {38.7, -9.1})->name == "Lisbon");
	WB_CHECK(nearest_city(cities, {35, 139})->name == "Tokyo");
	WB_CHECK(nearest_city({}, {0, 0}) == nullptr);
}

WB_TEST(geo_time_zone_city) {
	const char *tab =
		"# comment\n"
		"PT\t+3843-00908\tEurope/Lisbon\tPortugal (mainland)\n"
		"US\t+404251-0740023\tAmerica/New_York\tEastern (most areas)\n"
		"AU\t-3352+15113\tAustralia/Sydney\tNew South Wales (most areas)\n";
	auto lis = zone_position(tab, "Europe/Lisbon");
	WB_CHECK(lis && near(lis->lat, 38.7167) && near(lis->lon, -9.1333));
	auto ny = zone_position(tab, "America/New_York");
	WB_CHECK(ny && near(ny->lat, 40.7142) && near(ny->lon, -74.0064));
	auto syd = zone_position(tab, "Australia/Sydney");
	WB_CHECK(syd && near(syd->lat, -33.8667) && near(syd->lon, 151.2167));
	WB_CHECK(!zone_position(tab, "Europe/Nowhere"));
	WB_CHECK(zone_city("America/New_York") == "New York");
	WB_CHECK(zone_city("America/Argentina/Buenos_Aires") == "Buenos Aires");
}

WB_TEST(geo_degrees_as_typed) {
	/* A comma or a dot, whatever the locale. */
	WB_CHECK(parse_degrees("38.72", -90, 90) == 38.72);
	WB_CHECK(near(*parse_degrees("38,72", -90, 90), 38.72));
	WB_CHECK(near(*parse_degrees(" -9,14 ", -180, 180), -9.14));
	WB_CHECK(!parse_degrees("91", -90, 90));
	WB_CHECK(!parse_degrees("1.2.3", -90, 90) && !parse_degrees("", -90, 90) &&
			!parse_degrees("N38", -90, 90) && !parse_degrees("-", -90, 90));
	WB_CHECK(format_degrees(38.7167) == "38.72");
	WB_CHECK(format_degrees(-9.1333) == "-9.13");
	WB_CHECK(format_degrees(-0.004) == "0.00" || format_degrees(-0.004) == "-0.00");
	WB_CHECK(format_degrees(-0.5) == "-0.50");
}

WB_TEST(geo_shipped_data) {
	/* The files in data/geo load whole. */
	auto borders = load_borders(slurp(std::string(WB_SOURCE_ROOT) + "/data/geo/countries.bin"));
	WB_CHECK(borders.size() > 250);
	size_t points = 0;
	for (const auto &r : borders)
		points += r.size();
	WB_CHECK(points > 10000);
	auto cities = load_cities(slurp(std::string(WB_SOURCE_ROOT) + "/data/geo/cities.tsv"));
	WB_CHECK(cities.size() > 7000);
	auto lis = find_cities(cities, "lisbon", 1);
	WB_CHECK(lis.size() == 1 && lis[0]->country == "Portugal" && near(lis[0]->at.lat, 38.72));
	/* A cut-short file gives nothing, not half. */
	std::string b = slurp(std::string(WB_SOURCE_ROOT) + "/data/geo/countries.bin");
	WB_CHECK(load_borders(b.substr(0, b.size() - 3)).empty());
	WB_CHECK(load_borders("nope").empty());
}
