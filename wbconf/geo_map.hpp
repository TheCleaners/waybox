#ifndef WBCONF_GEO_MAP_HPP
#define WBCONF_GEO_MAP_HPP

/*
 * The offline world map (Natural Earth's outlines and cities, data/geo)
 * that wbconf's location picker and the location agent's card both draw:
 * the data, read once, and the map itself. Nothing here looks anything up.
 */
#include <optional>
#include <string>
#include <vector>

#include <cairo.h>
#include <gtk/gtk.h>

#include "waybox/geo.hpp"

namespace wbconf {

struct GeoData {
	std::vector<std::vector<wb::geo::Point>> borders;
	std::vector<wb::geo::City> cities;
	/* The time zone's place and city ("Lisbon"), if it's in zone1970.tab. */
	std::optional<wb::geo::Point> zone_at;
	std::string zone_city;
};

/* Read once ($WBCONF_GEO_DIR, else the installed data; $TZ, else
 * /etc/localtime, for the zone). */
const GeoData &geo_data();

/* What's shown: the place in the middle, and how many pixels a degree of
 * latitude takes. A degree of longitude takes `lon_scale` of that (1: the
 * picker's plain world map; cos(latitude): a region's true shape). */
struct MapView {
	wb::geo::Point center;
	double px_per_deg = 1;
	double lon_scale = 1;
	double x(const wb::geo::Point &p, double x0, double w) const {
		return x0 + w / 2 + (p.lon - center.lon) * px_per_deg * lon_scale;
	}
	double y(const wb::geo::Point &p, double y0, double h) const {
		return y0 + h / 2 - (p.lat - center.lat) * px_per_deg;
	}
};

/* The map in (x0, y0, w, h), clipped to it, in the widget's own color: the
 * sea, a grid, the land and its borders, and (with `cities`) the cities in
 * view, the biggest named. */
void draw_geo_map(cairo_t *cr, GtkWidget *w, double x0, double y0, double width, double height,
		const MapView &view, bool cities);

/* The outline that holds `p` (the country's, or the part of it), or none. */
const std::vector<wb::geo::Point> *ring_at(wb::geo::Point p);

}  // namespace wbconf

#endif /* WBCONF_GEO_MAP_HPP */
