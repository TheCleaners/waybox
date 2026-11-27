/*
 * A place on Earth, picked: a world map (countries' outlines, Natural
 * Earth's, drawn in the theme's colors) to click or drag a marker on, a
 * city search (offline: data/geo's cities), the time zone's own city
 * (tzdata), and under it the place in numbers and the nearest city.
 * For a program's latitude and longitude (an integration's Location=:
 * wlsunset's -l / -L): no network, no location service.
 */
#include "geo_picker.hpp"

#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

#include "waybox/geo.hpp"
#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

using wb::geo::Point;

struct GeoData {
	std::vector<std::vector<Point>> borders;
	std::vector<wb::geo::City> cities;
	std::optional<Point> zone_at;
	std::string zone_city;
};

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

/* Read once: the outlines, the cities, the time zone's place. */
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

struct Picker {
	std::function<std::optional<Point>()> get;
	std::function<void(Point)> set;
	GtkWidget *box = nullptr, *map = nullptr, *caption = nullptr, *search = nullptr;
	GtkWidget *results = nullptr, *popover = nullptr;
	std::vector<const wb::geo::City *> shown;
};

/* The map's rectangle in the widget: 2:1, centered. */
void map_box(GtkWidget *w, double *x, double *y, double *mw, double *mh) {
	double W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
	*mw = std::min(W, H * 2);
	*mh = *mw / 2;
	*x = (W - *mw) / 2;
	*y = (H - *mh) / 2;
}

void draw(GtkDrawingArea *area, cairo_t *cr, int, int, gpointer data) {
	auto *p = static_cast<Picker *>(data);
	GtkWidget *w = GTK_WIDGET(area);
	GdkRGBA fg;
	gtk_widget_get_color(w, &fg);
	double x0, y0, mw, mh;
	map_box(w, &x0, &y0, &mw, &mh);
	auto px = [&](const Point &pt) { return x0 + (pt.lon + 180) / 360 * mw; };
	auto py = [&](const Point &pt) { return y0 + (90 - pt.lat) / 180 * mh; };

	/* The sea, the grid every 30 degrees. */
	cairo_rectangle(cr, x0, y0, mw, mh);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
	for (int lon = -150; lon <= 150; lon += 30) {
		double x = px({0, static_cast<double>(lon)});
		cairo_move_to(cr, std::round(x) + 0.5, y0);
		cairo_line_to(cr, std::round(x) + 0.5, y0 + mh);
	}
	for (int lat = -60; lat <= 60; lat += 30) {
		double y = py({static_cast<double>(lat), 0});
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

	/* The place: a dot in the accent color, ringed so it shows on land. */
	if (auto at = p->get()) {
		GdkRGBA accent = {0.21, 0.52, 0.89, 1};
		gdk_rgba_parse(&accent, "#3584e4");
		double x = px(*at), y = py(*at);
		cairo_arc(cr, x, y, 7, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.25);
		cairo_fill(cr);
		cairo_arc(cr, x, y, 4, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 1);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 1.5);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
		cairo_stroke(cr);
	}
}

void refresh_caption(Picker *p) {
	auto at = p->get();
	if (!at) {
		gtk_label_set_text(GTK_LABEL(p->caption), _("No place yet: click where you are on the map, "
				"or find your city."));
		return;
	}
	std::string text = wb::geo::format_degrees(at->lat) + ", " + wb::geo::format_degrees(at->lon);
	if (const wb::geo::City *c = wb::geo::nearest_city(geo_data().cities, *at))
		text += std::string(" · ") + _("near") + " " + c->name + ", " + c->country;
	gtk_label_set_text(GTK_LABEL(p->caption), text.c_str());
}

void pick(Picker *p, Point at) {
	at.lat = std::clamp(at.lat, -90.0, 90.0);
	at.lon = std::clamp(at.lon, -180.0, 180.0);
	p->set(at);
	gtk_widget_queue_draw(p->map);
	refresh_caption(p);
}

/* A point in the widget, as a place (nullopt off the map). */
std::optional<Point> place_at(Picker *p, double x, double y) {
	double x0, y0, mw, mh;
	map_box(p->map, &x0, &y0, &mw, &mh);
	if (mw <= 0 || mh <= 0 || x < x0 || x > x0 + mw || y < y0 || y > y0 + mh)
		return std::nullopt;
	return Point{90 - (y - y0) / mh * 180, (x - x0) / mw * 360 - 180};
}

void fill_results(Picker *p) {
	while (GtkWidget *c = gtk_widget_get_first_child(p->results))
		gtk_list_box_remove(GTK_LIST_BOX(p->results), c);
	p->shown = wb::geo::find_cities(geo_data().cities, gtk_editable_get_text(GTK_EDITABLE(p->search)),
			8);
	for (const wb::geo::City *c : p->shown) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_margin_top(row, 3);
		gtk_widget_set_margin_bottom(row, 3);
		gtk_widget_set_margin_start(row, 4);
		GtkWidget *n = gtk_label_new(c->name.c_str());
		gtk_label_set_xalign(GTK_LABEL(n), 0);
		GtkWidget *k = gtk_label_new(c->country.c_str());
		gtk_label_set_xalign(GTK_LABEL(k), 0);
		gtk_widget_add_css_class(k, "dim-label");
		gtk_box_append(GTK_BOX(row), n);
		gtk_box_append(GTK_BOX(row), k);
		gtk_list_box_append(GTK_LIST_BOX(p->results), row);
	}
	/* (Only on screen: a folded section's search has nowhere to show it.) */
	if (p->shown.empty() || !gtk_widget_get_mapped(p->search))
		gtk_popover_popdown(GTK_POPOVER(p->popover));
	else
		gtk_popover_popup(GTK_POPOVER(p->popover));
}

}  // namespace

GtkWidget *geo_picker_new(std::function<std::optional<Point>()> get,
		std::function<void(Point)> set) {
	auto *p = new Picker;
	p->get = std::move(get);
	p->set = std::move(set);
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(p->box, "wb-geo-picker");
	g_object_set_data_full(G_OBJECT(p->box), "wb-geo", p,
			[](gpointer x) { delete static_cast<Picker *>(x); });

	/* Find a city; or the time zone's. */
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	p->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(p->search), _("Find a city"));
	gtk_editable_set_width_chars(GTK_EDITABLE(p->search), 24);
	gtk_box_append(GTK_BOX(bar), p->search);
	p->popover = gtk_popover_new();
	gtk_popover_set_autohide(GTK_POPOVER(p->popover), FALSE);
	gtk_popover_set_has_arrow(GTK_POPOVER(p->popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(p->popover), GTK_POS_BOTTOM);
	p->results = gtk_list_box_new();
	gtk_popover_set_child(GTK_POPOVER(p->popover), p->results);
	gtk_widget_set_parent(p->popover, p->search);
	g_signal_connect(p->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer d) {
		fill_results(static_cast<Picker *>(d));
	}), p);
	g_signal_connect(p->search, "stop-search", G_CALLBACK(+[](GtkSearchEntry *, gpointer d) {
		gtk_popover_popdown(GTK_POPOVER(static_cast<Picker *>(d)->popover));
	}), p);
	g_signal_connect(p->search, "activate", G_CALLBACK(+[](GtkSearchEntry *, gpointer d) {
		auto *p = static_cast<Picker *>(d);
		if (!p->shown.empty()) {
			pick(p, p->shown[0]->at);
			gtk_popover_popdown(GTK_POPOVER(p->popover));
		}
	}), p);
	g_signal_connect(p->results, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row,
			gpointer d) {
		auto *p = static_cast<Picker *>(d);
		int i = gtk_list_box_row_get_index(row);
		if (i >= 0 && static_cast<size_t>(i) < p->shown.size()) {
			pick(p, p->shown[static_cast<size_t>(i)]->at);
			gtk_editable_set_text(GTK_EDITABLE(p->search), "");
			gtk_popover_popdown(GTK_POPOVER(p->popover));
		}
	}), p);
	const GeoData &d = geo_data();
	if (d.zone_at) {
		std::string label = std::string(_("Use My Time Zone's City")) + " (" + d.zone_city + ")";
		GtkWidget *tz = gtk_button_new_with_label(label.c_str());
		gtk_widget_add_css_class(tz, "flat");
		gtk_widget_set_tooltip_text(tz, _("Your time zone's own city: close enough for sunset "
				"and sunrise, unless your zone is a wide one"));
		g_signal_connect(tz, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			if (geo_data().zone_at)
				pick(static_cast<Picker *>(d), *geo_data().zone_at);
		}), p);
		gtk_box_append(GTK_BOX(bar), tz);
	}
	gtk_box_append(GTK_BOX(p->box), bar);

	/* The map: click or drag the place. */
	p->map = gtk_drawing_area_new();
	gtk_widget_set_size_request(p->map, 480, 240);
	gtk_widget_set_hexpand(p->map, FALSE);
	gtk_widget_set_halign(p->map, GTK_ALIGN_START);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->map), draw, p, nullptr);
	gtk_widget_set_cursor_from_name(p->map, "crosshair");
	gtk_widget_set_tooltip_text(p->map, _("Click or drag where you are"));
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag *, double x, double y,
			gpointer d) {
		auto *p = static_cast<Picker *>(d);
		if (auto at = place_at(p, x, y))
			pick(p, *at);
	}), p);
	g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag *g, double dx, double dy,
			gpointer d) {
		auto *p = static_cast<Picker *>(d);
		double sx = 0;
		double sy = 0;
		gtk_gesture_drag_get_start_point(g, &sx, &sy);
		if (auto at = place_at(p, sx + dx, sy + dy))
			pick(p, *at);
	}), p);
	gtk_widget_add_controller(p->map, GTK_EVENT_CONTROLLER(drag));
	gtk_box_append(GTK_BOX(p->box), p->map);

	p->caption = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->caption), 0);
	gtk_widget_add_css_class(p->caption, "dim-label");
	gtk_box_append(GTK_BOX(p->box), p->caption);
	refresh_caption(p);
	return p->box;
}

void geo_picker_refresh(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr)
		return;
	gtk_widget_queue_draw(p->map);
	refresh_caption(p);
}

std::string geo_picker_caption(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	return p != nullptr ? gtk_label_get_text(GTK_LABEL(p->caption)) : "";
}

void geo_picker_find(GtkWidget *picker, const std::string &query) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr)
		return;
	gtk_editable_set_text(GTK_EDITABLE(p->search), query.c_str());
	fill_results(p);
	if (!p->shown.empty())
		pick(p, p->shown[0]->at);
	gtk_popover_popdown(GTK_POPOVER(p->popover));
}

bool geo_picker_use_zone(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr || !geo_data().zone_at)
		return false;
	pick(p, *geo_data().zone_at);
	return true;
}

void geo_picker_click(GtkWidget *picker, double fx, double fy) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr)
		return;
	/* As place_at reads a click that far across and down the map. */
	if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
		pick(p, Point{90 - fy * 180, fx * 360 - 180});
}

}  // namespace wbconf
