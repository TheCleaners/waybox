/*
 * The offline world map shared by the location picker and the location
 * agent's card (geo_map.hpp).
 */
#include "geo_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

namespace wbconf {

using wb::geo::Point;

namespace {

std::string slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

/* The time zone set: $TZ, else what /etc/localtime points to. */
std::string local_zone() {
	if (const char *tz = getenv("TZ"); tz != nullptr && *tz)
		return tz[0] == ':' ? tz + 1 : tz;
	char buf[512];
	ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
	if (n <= 0)
		return {};
	std::string link(buf, static_cast<size_t>(n));
	size_t at = link.find("zoneinfo/");
	return at == std::string::npos ? std::string() : link.substr(at + 9);
}

/* Grid lines: every 30 degrees, finer as it zooms in (at least four
 * across). */
double grid_step(double degrees_across) {
	for (double s : {30.0, 10.0, 5.0, 2.0, 1.0, 0.5, 0.25})
		if (degrees_across / s >= 4)
			return s;
	return 0.1;
}

}  // namespace

const GeoData &geo_data() {
	static std::unique_ptr<GeoData> d;
	if (d)
		return *d;
	d = std::make_unique<GeoData>();
	const char *env = getenv("WBCONF_GEO_DIR");  /* tests, development */
	std::string dir = env != nullptr && *env ? env : WB_GEO_DIR;
	d->borders = wb::geo::load_borders(slurp(dir + "/countries.bin"));
	d->cities = wb::geo::load_cities(slurp(dir + "/cities.tsv"));
	std::string zone = local_zone();
	const char *tzdir = getenv("TZDIR");
	std::string tab = slurp(std::string(tzdir != nullptr && *tzdir ? tzdir : "/usr/share/zoneinfo") +
			"/zone1970.tab");
	if (!zone.empty()) {
		d->zone_at = wb::geo::zone_position(tab, zone);
		d->zone_city = wb::geo::zone_city(zone);
	}
	return *d;
}

void draw_geo_map(cairo_t *cr, GtkWidget *w, double x0, double y0, double mw, double mh,
		const MapView &v, bool cities) {
	GdkRGBA fg;
	gtk_widget_get_color(w, &fg);
	auto px = [&](const Point &pt) { return v.x(pt, x0, mw); };
	auto py = [&](const Point &pt) { return v.y(pt, y0, mh); };
	double half_lon = mw / 2 / (v.px_per_deg * v.lon_scale), half_lat = mh / 2 / v.px_per_deg;
	double west = v.center.lon - half_lon, east = v.center.lon + half_lon;
	double south = v.center.lat - half_lat, north = v.center.lat + half_lat;

	cairo_save(cr);
	cairo_rectangle(cr, x0, y0, mw, mh);
	cairo_clip(cr);
	/* The sea, the grid. */
	cairo_rectangle(cr, x0, y0, mw, mh);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
	double step = grid_step(east - west);
	for (double lon = std::ceil(west / step) * step; lon <= east; lon += step) {
		double x = px({0, lon});
		cairo_move_to(cr, std::round(x) + 0.5, y0);
		cairo_line_to(cr, std::round(x) + 0.5, y0 + mh);
	}
	for (double lat = std::ceil(south / step) * step; lat <= north; lat += step) {
		double y = py({lat, 0});
		cairo_move_to(cr, x0, std::round(y) + 0.5);
		cairo_line_to(cr, x0 + mw, std::round(y) + 0.5);
	}
	cairo_stroke(cr);

	/* The land, and the countries' borders. */
	for (const auto &ring : geo_data().borders) {
		if (ring.empty())
			continue;
		cairo_move_to(cr, px(ring[0]), py(ring[0]));
		for (size_t i = 1; i < ring.size(); ++i)
			cairo_line_to(cr, px(ring[i]), py(ring[i]));
		cairo_close_path(cr);
	}
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.16);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, 0.7);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.38);
	cairo_stroke(cr);

	/* The cities, to tell where; the biggest named, none over another. */
	if (cities) {
		std::vector<std::array<double, 4>> taken;
		int dots = 0, names = 0;
		PangoLayout *layout = gtk_widget_create_pango_layout(w, nullptr);
		PangoAttrList *small = pango_attr_list_new();
		pango_attr_list_insert(small, pango_attr_scale_new(0.82));
		pango_layout_set_attributes(layout, small);
		pango_attr_list_unref(small);
		for (const wb::geo::City &c : geo_data().cities) {  /* the most populous first */
			if (c.at.lon < west || c.at.lon > east || c.at.lat < south || c.at.lat > north)
				continue;
			if (++dots > 120)
				break;
			double x = px(c.at), y = py(c.at);
			cairo_arc(cr, x, y, 1.8, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
			cairo_fill(cr);
			if (names >= 24)
				continue;
			pango_layout_set_text(layout, c.name.c_str(), -1);
			int lw = 0, lh = 0;
			pango_layout_get_pixel_size(layout, &lw, &lh);
			std::array<double, 4> box = {x + 4, y - lh / 2.0, x + 4 + lw, y + lh / 2.0};
			bool clear = std::none_of(taken.begin(), taken.end(), [&box](const auto &t) {
				return box[0] < t[2] + 4 && t[0] < box[2] + 4 && box[1] < t[3] && t[1] < box[3];
			});
			if (!clear)
				continue;
			taken.push_back(box);
			++names;
			cairo_move_to(cr, box[0], box[1]);
			cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.75);
			pango_cairo_show_layout(cr, layout);
		}
		g_object_unref(layout);
	}
	cairo_restore(cr);
	/* Nothing left open (the names leave a current point, which the
	 * caller's next arc would join with a line). */
	cairo_new_path(cr);
}

const std::vector<Point> *ring_at(Point p) {
	for (const auto &ring : geo_data().borders) {
		bool inside = false;
		for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
			const Point &a = ring[i], &b = ring[j];
			if ((a.lat > p.lat) != (b.lat > p.lat) &&
					p.lon < (b.lon - a.lon) * (p.lat - a.lat) / (b.lat - a.lat) + a.lon)
				inside = !inside;
		}
		if (inside)
			return &ring;
	}
	return nullptr;
}

}  // namespace wbconf
