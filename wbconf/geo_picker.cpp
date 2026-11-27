/*
 * A place on Earth, picked: a world map (countries' outlines, Natural
 * Earth's, drawn in the theme's colors) to click or drag a marker on, a
 * city search (offline: data/geo's cities), the time zone's own city
 * (tzdata), and under it the place in numbers and the nearest city.
 * For a program's latitude and longitude (an integration's Location=:
 * wlsunset's -l / -L): offline. The crosshair asks the system where it
 * is, only when clicked: the xdg-desktop-portal Location portal (which
 * may ask the user first), where there is one.
 */
#include "geo_picker.hpp"

#include <algorithm>
#include <array>
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
	/* Asking the Location portal: its session, the signals waited for. */
	GtkWidget *locate = nullptr;
	/* The view: zoom (1: the whole world) and the place in its middle. */
	double zoom = 1;
	Point view;
	GtkWidget *zoom_in = nullptr, *zoom_out = nullptr;
	/* A drag: of the place (pressed on it, or nothing to pan), or of the
	 * map; a press that didn't move is a click. */
	bool dragging_place = false, moved = false;
	Point view_at_press;
	double pointer_x = -1, pointer_y = -1;
	GDBusConnection *bus = nullptr;
	std::string session;
	guint on_location = 0, on_response = 0, timer = 0;
	~Picker();
};

constexpr const char *kPortal = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kLocation = "org.freedesktop.portal.Location";

/* The session bus, if there's one (none in tests: disabled). */
GDBusConnection *session_bus() {
	static GDBusConnection *bus = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
	}
	return bus;
}

/* Whether GeoClue (what the portal asks) is on the system bus, running
 * or there to start: a portal built with it offers Location even where
 * it isn't installed, and then can only fail. WBCONF_GEOCLUE=1: taken as
 * there (tests, whose stand-in portal answers itself). */
bool geoclue_present() {
	const char *forced = g_getenv("WBCONF_GEOCLUE");
	if (forced != nullptr)
		return g_strcmp0(forced, "1") == 0;
	GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
	if (sys == nullptr)
		return false;
	bool found = false;
	for (const char *method : {"ListNames", "ListActivatableNames"}) {
		GVariant *v = g_dbus_connection_call_sync(sys, "org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus", method, nullptr,
				G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
		if (v == nullptr)
			continue;
		GVariantIter *it = nullptr;
		const char *name = nullptr;
		g_variant_get(v, "(as)", &it);
		while (!found && g_variant_iter_next(it, "&s", &name))
			found = g_strcmp0(name, "org.freedesktop.GeoClue2") == 0;
		g_variant_iter_free(it);
		g_variant_unref(v);
	}
	g_object_unref(sys);
	return found;
}

/* Whether a portal offers Location (its interface's version answers) with
 * GeoClue behind it. */
bool portal_has_location(GDBusConnection *bus) {
	if (bus == nullptr || !geoclue_present())
		return false;
	GVariant *v = g_dbus_connection_call_sync(bus, kPortal, kPortalPath,
			"org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", kLocation, "version"),
			G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
	if (v == nullptr)
		return false;
	g_variant_unref(v);
	return true;
}

void stop_locate(Picker *p, const char *why);

Picker::~Picker() {
	stop_locate(this, nullptr);
}

/* "Where am I?" over: signals let go, the session closed, the button
 * back; `why` (if any) said under the map. */
void stop_locate(Picker *p, const char *why) {
	if (p->bus != nullptr) {
		if (p->on_location != 0)
			g_dbus_connection_signal_unsubscribe(p->bus, p->on_location);
		if (p->on_response != 0)
			g_dbus_connection_signal_unsubscribe(p->bus, p->on_response);
		if (!p->session.empty())
			g_dbus_connection_call(p->bus, kPortal, p->session.c_str(),
					"org.freedesktop.portal.Session", "Close", nullptr, nullptr,
					G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
	}
	p->on_location = p->on_response = 0;
	p->session.clear();
	if (p->timer != 0) {
		g_source_remove(p->timer);
		p->timer = 0;
	}
	if (p->locate != nullptr)
		gtk_widget_set_sensitive(p->locate, TRUE);
	if (why != nullptr && p->caption != nullptr)
		gtk_label_set_text(GTK_LABEL(p->caption), why);
}

void refresh_caption(Picker *p);
void pick(Picker *p, Point at);
void show_place(Picker *p, Point at);

/* Ask the Location portal where this is: a session, started; the place
 * comes in a signal (LocationUpdated), or a refusal in the request's
 * Response. Neighborhood accuracy: plenty for sunset and sunrise. */
void start_locate(Picker *p) {
	p->bus = session_bus();
	if (p->bus == nullptr || !p->session.empty())
		return;
	static int count = 0;
	++count;
	std::string sender = g_dbus_connection_get_unique_name(p->bus) + 1;  /* without ':' */
	std::replace(sender.begin(), sender.end(), '.', '_');
	std::string token = "wbconf_" + std::to_string(getpid()) + "_" + std::to_string(count);
	p->session = std::string(kPortalPath) + "/session/" + sender + "/" + token;
	std::string request = std::string(kPortalPath) + "/request/" + sender + "/" + token;
	gtk_widget_set_sensitive(p->locate, FALSE);
	gtk_label_set_text(GTK_LABEL(p->caption), _("Asking the system where this is\u2026"));

	/* Listened for before asking, so no answer is missed. */
	p->on_location = g_dbus_connection_signal_subscribe(p->bus, kPortal, kLocation,
			"LocationUpdated", kPortalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
			[](GDBusConnection *, const char *, const char *, const char *, const char *,
					GVariant *params, gpointer d) {
		auto *p = static_cast<Picker *>(d);
		const char *session = nullptr;
		GVariant *loc = nullptr;
		g_variant_get(params, "(&o@a{sv})", &session, &loc);
		double lat = 0, lon = 0;
		bool ok = session != nullptr && p->session == session &&
				g_variant_lookup(loc, "Latitude", "d", &lat) &&
				g_variant_lookup(loc, "Longitude", "d", &lon);
		g_variant_unref(loc);
		if (!ok)
			return;
		stop_locate(p, nullptr);
		show_place(p, Point{lat, lon});
		pick(p, Point{lat, lon});
	}, p, nullptr);
	p->on_response = g_dbus_connection_signal_subscribe(p->bus, kPortal,
			"org.freedesktop.portal.Request", "Response", request.c_str(), nullptr,
			G_DBUS_SIGNAL_FLAGS_NONE, [](GDBusConnection *, const char *, const char *,
					const char *, const char *, GVariant *params, gpointer d) {
		guint32 response = 0;
		g_variant_get(params, "(u@a{sv})", &response, nullptr);
		if (response != 0)
			stop_locate(static_cast<Picker *>(d), _("The system didn't say where this is "
					"(not allowed, or it doesn't know). Pick it on the map."));
	}, p, nullptr);
	p->timer = g_timeout_add_seconds(30, [](gpointer d) {
		auto *p = static_cast<Picker *>(d);
		p->timer = 0;
		stop_locate(p, _("No answer from the system's location service. Pick it on the map."));
		return G_SOURCE_REMOVE;
	}, p);

	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));
	g_variant_builder_add(&opts, "{sv}", "accuracy", g_variant_new_uint32(3));  /* neighborhood */
	GError *err = nullptr;
	GVariant *r = g_dbus_connection_call_sync(p->bus, kPortal, kPortalPath, kLocation,
			"CreateSession", g_variant_new("(a{sv})", &opts), G_VARIANT_TYPE("(o)"),
			G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &err);
	if (r == nullptr) {
		g_clear_error(&err);
		stop_locate(p, _("The system's location service isn't there. Pick it on the map."));
		return;
	}
	g_variant_unref(r);
	GVariantBuilder start;
	g_variant_builder_init(&start, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&start, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
	r = g_dbus_connection_call_sync(p->bus, kPortal, kPortalPath, kLocation, "Start",
			g_variant_new("(osa{sv})", p->session.c_str(), "", &start), G_VARIANT_TYPE("(o)"),
			G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &err);
	if (r == nullptr) {
		g_clear_error(&err);
		stop_locate(p, _("The system's location service didn't start. Pick it on the map."));
		return;
	}
	g_variant_unref(r);
}

/* The map's rectangle in the widget: 2:1, centered. */
void map_box(GtkWidget *w, double *x, double *y, double *mw, double *mh) {
	double W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
	*mw = std::min(W, H * 2);
	*mh = *mw / 2;
	*x = (W - *mw) / 2;
	*y = (H - *mh) / 2;
}

constexpr double kMaxZoom = 64;

/* The view kept on the world (no space beyond its edges), the buttons
 * off at the ends. */
void clamp_view(Picker *p) {
	p->zoom = std::clamp(p->zoom, 1.0, kMaxZoom);
	double hw = 180 / p->zoom, hh = 90 / p->zoom;
	p->view.lon = std::clamp(p->view.lon, -180 + hw, 180 - hw);
	p->view.lat = std::clamp(p->view.lat, -90 + hh, 90 - hh);
	if (p->zoom_in != nullptr) {
		gtk_widget_set_sensitive(p->zoom_in, p->zoom < kMaxZoom);
		gtk_widget_set_sensitive(p->zoom_out, p->zoom > 1);
	}
}

/* The place at a fraction across and down the view, and back. */
Point at_fraction(const Picker *p, double fx, double fy) {
	return Point{p->view.lat + (0.5 - fy) * 180 / p->zoom, p->view.lon + (fx - 0.5) * 360 / p->zoom};
}
void fraction_of(const Picker *p, Point at, double *fx, double *fy) {
	*fx = 0.5 + (at.lon - p->view.lon) * p->zoom / 360;
	*fy = 0.5 - (at.lat - p->view.lat) * p->zoom / 180;
}

/* Zoomed by `factor`, the place at (fx, fy) staying under it (the
 * pointer's, for the wheel; the middle, for the buttons). */
void zoom_at(Picker *p, double factor, double fx, double fy) {
	Point keep = at_fraction(p, fx, fy);
	p->zoom = std::clamp(p->zoom * factor, 1.0, kMaxZoom);
	p->view.lon = keep.lon - (fx - 0.5) * 360 / p->zoom;
	p->view.lat = keep.lat - (0.5 - fy) * 180 / p->zoom;
	clamp_view(p);
	gtk_widget_queue_draw(p->map);
}

/* The map moved by a fraction of the view (as a drag moves it). */
void pan_by(Picker *p, double dfx, double dfy) {
	p->view.lon -= dfx * 360 / p->zoom;
	p->view.lat += dfy * 180 / p->zoom;
	clamp_view(p);
	gtk_widget_queue_draw(p->map);
}

/* A place shown: in the middle, near enough to see where (a city, the
 * time zone's, the system's answer). */
void show_place(Picker *p, Point at) {
	p->view = at;
	p->zoom = std::max(p->zoom, 8.0);
	clamp_view(p);
}

/* Grid lines: every 30 degrees, finer as it zooms in. */
double grid_step(double zoom) {
	for (double s : {30.0, 10.0, 5.0, 2.0, 1.0, 0.5, 0.25})
		if (360 / zoom / s >= 4)
			return s;
	return 0.1;
}

void draw(GtkDrawingArea *area, cairo_t *cr, int, int, gpointer data) {
	auto *p = static_cast<Picker *>(data);
	GtkWidget *w = GTK_WIDGET(area);
	GdkRGBA fg;
	gtk_widget_get_color(w, &fg);
	double x0, y0, mw, mh;
	map_box(w, &x0, &y0, &mw, &mh);
	double scale = mw * p->zoom / 360;  /* pixels a degree */
	double cx = x0 + mw / 2, cy = y0 + mh / 2;
	auto px = [&](const Point &pt) { return cx + (pt.lon - p->view.lon) * scale; };
	auto py = [&](const Point &pt) { return cy - (pt.lat - p->view.lat) * scale; };
	double west = p->view.lon - 180 / p->zoom, east = p->view.lon + 180 / p->zoom;
	double south = p->view.lat - 90 / p->zoom, north = p->view.lat + 90 / p->zoom;

	cairo_save(cr);
	cairo_rectangle(cr, x0, y0, mw, mh);
	cairo_clip(cr);
	/* The sea, the grid. */
	cairo_rectangle(cr, x0, y0, mw, mh);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.04);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
	double step = grid_step(p->zoom);
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

	/* Zoomed in: the cities, to tell where; the biggest named, none
	 * over another. */
	if (p->zoom >= 4) {
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
	cairo_restore(cr);
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

/* A point in the widget, as a fraction of the view (false off the map). */
bool fraction_at(Picker *p, double x, double y, double *fx, double *fy) {
	double x0, y0, mw, mh;
	map_box(p->map, &x0, &y0, &mw, &mh);
	if (mw <= 0 || mh <= 0)
		return false;
	*fx = (x - x0) / mw;
	*fy = (y - y0) / mh;
	return *fx >= 0 && *fx <= 1 && *fy >= 0 && *fy <= 1;
}

/* A point in the widget, as a place (nullopt off the map). */
std::optional<Point> place_at(Picker *p, double x, double y) {
	double fx = 0, fy = 0;
	if (!fraction_at(p, x, y, &fx, &fy))
		return std::nullopt;
	return at_fraction(p, fx, fy);
}

/* Whether the point is on the place's dot (to drag it, not the map). */
bool on_place(Picker *p, double x, double y) {
	auto at = p->get();
	double x0, y0, mw, mh;
	map_box(p->map, &x0, &y0, &mw, &mh);
	if (!at || mw <= 0)
		return false;
	double fx = 0, fy = 0;
	fraction_of(p, *at, &fx, &fy);
	return std::hypot(x0 + fx * mw - x, y0 + fy * mh - y) <= 10;
}

void on_drag_begin(GtkGestureDrag *, double x, double y, gpointer d) {
	auto *p = static_cast<Picker *>(d);
	p->moved = false;
	p->view_at_press = p->view;
	/* Nothing to pan (the whole world shown): a drag is of the place. */
	p->dragging_place = on_place(p, x, y) || p->zoom <= 1;
	if (p->dragging_place)
		if (auto at = place_at(p, x, y))
			pick(p, *at);
}

void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d) {
	auto *p = static_cast<Picker *>(d);
	double sx = 0;
	double sy = 0;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	if (p->dragging_place) {
		if (auto at = place_at(p, sx + dx, sy + dy))
			pick(p, *at);
		return;
	}
	if (!p->moved && std::hypot(dx, dy) < 4)
		return;
	p->moved = true;
	gtk_widget_set_cursor_from_name(p->map, "grabbing");
	double x0, y0, mw, mh;
	map_box(p->map, &x0, &y0, &mw, &mh);
	if (mw <= 0)
		return;
	p->view = p->view_at_press;
	pan_by(p, dx / mw, dy / mh);
}

void on_drag_end(GtkGestureDrag *g, double, double, gpointer d) {
	auto *p = static_cast<Picker *>(d);
	gtk_widget_set_cursor_from_name(p->map, "crosshair");
	if (p->dragging_place || p->moved)
		return;
	/* Pressed and let go where it was: a click, the place put there. */
	double sx = 0;
	double sy = 0;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	if (auto at = place_at(p, sx, sy))
		pick(p, *at);
}

gboolean on_scroll(GtkEventControllerScroll *c, double, double dy, gpointer d) {
	auto *p = static_cast<Picker *>(d);
	double fx = 0.5, fy = 0.5;
	if (!fraction_at(p, p->pointer_x, p->pointer_y, &fx, &fy)) {
		fx = 0.5;
		fy = 0.5;
	}
	/* A wheel's notch: twice as near (or far); a touchpad's, smoothly. */
	double notches = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL ? dy : dy / 40;
	/* (Taken even with no further to go: the page scrolling on from the
	 * map's limits jars.) */
	zoom_at(p, std::pow(2.0, -notches), fx, fy);
	return TRUE;
}

void on_motion(GtkEventControllerMotion *, double x, double y, gpointer d) {
	auto *p = static_cast<Picker *>(d);
	p->pointer_x = x;
	p->pointer_y = y;
	gtk_widget_set_cursor_from_name(p->map, on_place(p, x, y) ? "grab" : "crosshair");
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
	/* The crosshair: where this is, asked of the system (only on a click). */
	p->locate = gtk_button_new_from_icon_name("find-location-symbolic");
	if (portal_has_location(session_bus())) {
		gtk_widget_set_tooltip_text(p->locate, _("Find Where I Am: asks the system's location "
				"service, which may ask you first"));
	} else {
		gtk_widget_set_sensitive(p->locate, FALSE);
		gtk_widget_set_tooltip_text(p->locate, _("No location service to ask here (it "
				"needs GeoClue and xdg-desktop-portal): pick it on the map or by city"));
	}
	g_signal_connect(p->locate, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		start_locate(static_cast<Picker *>(d));
	}), p);
	gtk_box_append(GTK_BOX(bar), p->locate);
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
			show_place(p, p->shown[0]->at);
			pick(p, p->shown[0]->at);
			gtk_popover_popdown(GTK_POPOVER(p->popover));
		}
	}), p);
	g_signal_connect(p->results, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row,
			gpointer d) {
		auto *p = static_cast<Picker *>(d);
		int i = gtk_list_box_row_get_index(row);
		if (i >= 0 && static_cast<size_t>(i) < p->shown.size()) {
			show_place(p, p->shown[static_cast<size_t>(i)]->at);
			pick(p, p->shown[static_cast<size_t>(i)]->at);
			gtk_editable_set_text(GTK_EDITABLE(p->search), "");
			gtk_popover_popdown(GTK_POPOVER(p->popover));
		}
	}), p);
	const GeoData &d = geo_data();
	if (d.zone_at) {
		/* A button that looks one: framed, a clock, the city named. */
		std::string label = std::string(_("Use Time Zone's City:")) + " " + d.zone_city;
		GtkWidget *tz = gtk_button_new();
		GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("preferences-system-time-symbolic"));
		gtk_box_append(GTK_BOX(content), gtk_label_new(label.c_str()));
		gtk_button_set_child(GTK_BUTTON(tz), content);
		gtk_widget_set_tooltip_text(tz, _("Your time zone's own city: close enough for sunset "
				"and sunrise, unless your zone is a wide one"));
		g_signal_connect(tz, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			if (geo_data().zone_at) {
				show_place(static_cast<Picker *>(d), *geo_data().zone_at);
				pick(static_cast<Picker *>(d), *geo_data().zone_at);
			}
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
	gtk_widget_set_tooltip_text(p->map, _("Click where you are, or drag the dot; drag the map "
			"to move it, the wheel or + and \u2212 to zoom"));
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), p);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), p);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), p);
	gtk_widget_add_controller(p->map, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *scroll = gtk_event_controller_scroll_new(
			GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), p);
	gtk_widget_add_controller(p->map, scroll);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
	gtk_widget_add_controller(p->map, motion);
	/* + and \u2212, over the map's corner. */
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), p->map);
	gtk_widget_set_halign(overlay, GTK_ALIGN_START);
	GtkWidget *zoom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(zoom, "linked");
	gtk_widget_set_halign(zoom, GTK_ALIGN_END);
	gtk_widget_set_valign(zoom, GTK_ALIGN_START);
	gtk_widget_set_margin_top(zoom, 6);
	gtk_widget_set_margin_end(zoom, 6);
	p->zoom_in = gtk_button_new_from_icon_name("list-add-symbolic");
	p->zoom_out = gtk_button_new_from_icon_name("list-remove-symbolic");
	gtk_widget_set_tooltip_text(p->zoom_in, _("Zoom In"));
	gtk_widget_set_tooltip_text(p->zoom_out, _("Zoom Out"));
	for (GtkWidget *b : {p->zoom_in, p->zoom_out}) {
		gtk_widget_add_css_class(b, "wb-geo-zoom");
		gtk_box_append(GTK_BOX(zoom), b);
	}
	g_signal_connect(p->zoom_in, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		zoom_at(static_cast<Picker *>(d), 2, 0.5, 0.5);
	}), p);
	g_signal_connect(p->zoom_out, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		zoom_at(static_cast<Picker *>(d), 0.5, 0.5, 0.5);
	}), p);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), zoom);
	clamp_view(p);
	gtk_box_append(GTK_BOX(p->box), overlay);

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
	if (auto at = p->get()) {
		double fx = 0, fy = 0;
		fraction_of(p, *at, &fx, &fy);
		if (fx < 0 || fx > 1 || fy < 0 || fy > 1) {
			p->view = *at;
			clamp_view(p);
		}
	}
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
	if (!p->shown.empty()) {
		show_place(p, p->shown[0]->at);
		pick(p, p->shown[0]->at);
		gtk_editable_set_text(GTK_EDITABLE(p->search), "");  /* as a pick from the list */
	}
	gtk_popover_popdown(GTK_POPOVER(p->popover));
}

bool geo_picker_locate(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr || !gtk_widget_get_sensitive(p->locate))
		return false;
	start_locate(p);
	return true;
}

bool geo_picker_use_zone(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr || !geo_data().zone_at)
		return false;
	show_place(p, *geo_data().zone_at);
	pick(p, *geo_data().zone_at);
	return true;
}

void geo_picker_click(GtkWidget *picker, double fx, double fy) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr)
		return;
	/* As a click that far across and down the view. */
	if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
		pick(p, at_fraction(p, fx, fy));
}

void geo_picker_zoom(GtkWidget *picker, double steps, double fx, double fy) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p != nullptr)
		zoom_at(p, std::pow(2.0, steps), fx, fy);
}

void geo_picker_pan(GtkWidget *picker, double dfx, double dfy) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p != nullptr)
		pan_by(p, dfx, dfy);
}

std::string geo_picker_view(GtkWidget *picker) {
	auto *p = static_cast<Picker *>(g_object_get_data(G_OBJECT(picker), "wb-geo"));
	if (p == nullptr)
		return "";
	char buf[96];
	g_snprintf(buf, sizeof buf, "%.2f %s %s", p->zoom, wb::geo::format_degrees(p->view.lat).c_str(),
			wb::geo::format_degrees(p->view.lon).c_str());
	return buf;
}

}  // namespace wbconf
