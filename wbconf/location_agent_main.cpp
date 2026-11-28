/*
 * waybox-location-agent: GeoClue's agent for the session (#363). When an
 * app asks GeoClue where you are, GeoClue asks its agent: this one shows a
 * card, "App wants to know where you are", how near, Allow / Deny, and how
 * long the answer holds (this time, the session, always). Answers kept
 * always go in the xdg permission store's "location" table, where
 * xdg-desktop-portal keeps its own, so the two agree.
 *
 * Registers as org.waybox.LocationAgent; where GeoClue's config doesn't
 * list that, as geoclue-demo-agent, which every distribution's does (the
 * ID is self-declared, not a security boundary; see #363).
 *
 * Tests: WAYBOX_LOCATION_TEST_ANSWER="allow:once" (or deny; once,
 * session, always; "every" too) answers each card as shown, after printing
 * its wording ("card: ..." on stderr); "@FILE": read from FILE each time;
 * "wait": not answered (GeoClue stopping meanwhile, say).
 */
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#include <cmath>
#include <ctime>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "geo_map.hpp"
#include "waybox/location_agent.hpp"

namespace loc = wb::location;

namespace {

constexpr const char *kAppId = "org.waybox.LocationAgent";
constexpr const char *kFallbackId = "geoclue-demo-agent";

constexpr const char *kAgentXml =
		"<node><interface name='org.freedesktop.GeoClue2.Agent'>"
		"<property name='MaxAccuracyLevel' type='u' access='read'/>"
		"<method name='AuthorizeApp'>"
		"<arg name='desktop_id' type='s' direction='in'/>"
		"<arg name='req_accuracy_level' type='u' direction='in'/>"
		"<arg name='authorized' type='b' direction='out'/>"
		"<arg name='allowed_accuracy_level' type='u' direction='out'/>"
		"</method></interface></node>";

constexpr const char *kCss =
		"window.wb-location { background: transparent; }"
		".wb-location-card { background-color: @theme_bg_color; color: @theme_fg_color;"
		"  border-radius: 14px; padding: 20px 22px 18px 22px; margin: 16px;"
		"  border: 1px solid alpha(@theme_fg_color, 0.12);"
		"  box-shadow: 0 4px 16px alpha(black, 0.30); }"
		".wb-location-title { font-size: 1.25em; }"
		".wb-location-near { opacity: 0.75; }"
		".wb-location-note { opacity: 0.6; font-size: 0.9em; }"
		".wb-location-map { opacity: 0.85; }";

struct Request {
	std::string app;
	uint32_t level = 0;
	GDBusMethodInvocation *invocation = nullptr;
};

struct Agent {
	GtkApplication *app = nullptr;
	GDBusConnection *system = nullptr;
	std::string registered_as;
	std::map<std::string, loc::Answer> session;
	std::optional<loc::Answer> session_all;  /* "every app, this session" */
	std::deque<Request> queue;  /* the first is the one shown */
	GtkWindow *card = nullptr;
	GtkDropDown *hold = nullptr;
	GtkCheckButton *every = nullptr;
	guint watch = 0;
};

Agent g;

/* ---- The permission store (xdg-desktop-portal's) ---- */

GDBusConnection *session_bus() {
	return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

/* Its "location" table: app -> {accuracy name, last used}. Empty if it
 * isn't there (no portal installed: nothing kept always). */
std::map<std::string, std::vector<std::string>> store_lookup() {
	std::map<std::string, std::vector<std::string>> out;
	GDBusConnection *bus = session_bus();
	if (bus == nullptr)
		return out;
	GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.impl.portal.PermissionStore",
			"/org/freedesktop/impl/portal/PermissionStore",
			"org.freedesktop.impl.portal.PermissionStore", "Lookup",
			g_variant_new("(ss)", "location", "location"), G_VARIANT_TYPE("(a{sas}v)"),
			G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
	if (r != nullptr) {
		GVariant *perms = g_variant_get_child_value(r, 0);
		GVariantIter it;
		g_variant_iter_init(&it, perms);
		const char *app = nullptr;
		GVariant *strv = nullptr;
		while (g_variant_iter_next(&it, "{&s@as}", &app, &strv)) {
			gsize n = 0;
			const char **v = g_variant_get_strv(strv, &n);
			out[app] = std::vector<std::string>(v, v + n);
			g_free(v);
			g_variant_unref(strv);
		}
		g_variant_unref(perms);
		g_variant_unref(r);
	}
	g_object_unref(bus);
	return out;
}

void store_set(const std::string &app, const loc::Answer &a) {
	GDBusConnection *bus = session_bus();
	if (bus == nullptr)
		return;
	std::vector<std::string> entry = loc::store_entry(a, static_cast<int64_t>(time(nullptr)));
	std::vector<const char *> v;
	for (const std::string &s : entry)
		v.push_back(s.c_str());
	GError *err = nullptr;
	GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.impl.portal.PermissionStore",
			"/org/freedesktop/impl/portal/PermissionStore",
			"org.freedesktop.impl.portal.PermissionStore", "SetPermission",
			g_variant_new("(sbss^as)", "location", TRUE, "location", app.c_str(),
					const_cast<char **>(v.data())),
			nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
	if (r == nullptr) {
		g_printerr("waybox-location-agent: can't keep the answer for %s: %s\n", app.c_str(),
				err->message);
		g_error_free(err);
	} else {
		g_variant_unref(r);
	}
	g_object_unref(bus);
}

/* ---- Answering ---- */

void show_next();

void reply(Request &r, const loc::Answer &a) {
	g_dbus_method_invocation_return_value(r.invocation,
			g_variant_new("(bu)", a.allow, a.allow ? a.level : 0u));
	r.invocation = nullptr;
	g_printerr("waybox-location-agent: %s %s (%s)\n", a.allow ? "allowed" : "denied",
			r.app.c_str(), loc::level_name(a.allow ? a.level : 0).c_str());
}

/* The card's answer: given, kept as long as asked, the next card shown. */
void answer(bool allow) {
	if (g.queue.empty())
		return;
	Request r = g.queue.front();
	g.queue.pop_front();
	loc::Hold hold = static_cast<loc::Hold>(gtk_drop_down_get_selected(g.hold));
	bool every = gtk_check_button_get_active(g.every);
	loc::Answer a{allow, allow ? r.level : loc::kNone};
	if (every)
		g.session_all = a;
	else if (hold == loc::Hold::Session)
		g.session[r.app] = a;
	else if (hold == loc::Hold::Always)
		store_set(r.app, a);
	reply(r, a);
	if (g.card != nullptr) {
		GtkWindow *w = g.card;
		g.card = nullptr;
		gtk_window_destroy(w);
	}
	/* Others asked meanwhile may be answered now ("every app"). */
	show_next();
}

/* An answer kept for it (the session's, "every app", or the store's). */
std::optional<loc::Answer> kept(const Request &r) {
	if (g.session_all) {
		loc::Answer a = *g.session_all;
		if (a.allow)
			a.level = r.level;
		return a;
	}
	return loc::kept_answer(r.app, r.level, g.session, store_lookup());
}

/* ---- The card ---- */

struct AppLook {
	std::string name;
	GIcon *icon = nullptr;  /* owned */
	bool known = false;
};

AppLook app_look(const std::string &id) {
	AppLook look;
	look.name = id;
	for (const std::string &file : {id + ".desktop", id}) {
		GDesktopAppInfo *info = g_desktop_app_info_new(file.c_str());
		if (info == nullptr)
			continue;
		look.name = g_app_info_get_display_name(G_APP_INFO(info));
		if (GIcon *icon = g_app_info_get_icon(G_APP_INFO(info)))
			look.icon = G_ICON(g_object_ref(icon));
		look.known = true;
		g_object_unref(info);
		break;
	}
	if (look.icon == nullptr)
		look.icon = g_themed_icon_new("find-location");
	return look;
}

/* ---- The map: how near, around the time zone's city ---- */

/* How far off an answer at `level` may be, in km, as GeoClue's levels go
 * (a city ~15 km, a neighborhood ~1 km, a street ~100 m); 0 for exact. */
double radius_km(uint32_t level) {
	if (level >= loc::kExact)
		return 0;
	if (level >= loc::kStreet)
		return 0.1;
	if (level >= loc::kNeighborhood)
		return 1;
	return 15;
}

/* Drawn from what's here (the time zone's place, Natural Earth's map),
 * before anything is looked up: the country lit (a country's worth), a
 * circle a city's size, or a pin (nearer). */
void draw_map(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer level_p) {
	auto level = static_cast<uint32_t>(GPOINTER_TO_UINT(level_p));
	const wbconf::GeoData &d = wbconf::geo_data();
	if (!d.zone_at)
		return;
	GtkWidget *w = GTK_WIDGET(area);
	wb::geo::Point at = *d.zone_at;
	bool country = level < loc::kCity;
	wbconf::MapView view;
	view.center = at;
	view.lon_scale = std::max(0.2, std::cos(at.lat * M_PI / 180));
	/* A country: some 30 degrees across; nearer: a degree high (about
	 * 110 km), the city's neighbors around it. */
	view.px_per_deg = country ? width / (30.0 * view.lon_scale) : height / 1.0;
	double r = 8;
	cairo_new_path(cr);
	cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
	cairo_arc(cr, width - r, r, r, 1.5 * M_PI, 2 * M_PI);
	cairo_arc(cr, width - r, height - r, r, 0, 0.5 * M_PI);
	cairo_arc(cr, r, height - r, r, 0.5 * M_PI, M_PI);
	cairo_close_path(cr);
	cairo_clip(cr);
	wbconf::draw_geo_map(cr, w, 0, 0, width, height, view, !country);

	GdkRGBA accent;
	gdk_rgba_parse(&accent, "#3584e4");
	double x = view.x(at, 0, width), y = view.y(at, 0, height);
	if (country) {
		if (const auto *ring = wbconf::ring_at(at)) {
			cairo_move_to(cr, view.x((*ring)[0], 0, width), view.y((*ring)[0], 0, height));
			for (const auto &pt : *ring)
				cairo_line_to(cr, view.x(pt, 0, width), view.y(pt, 0, height));
			cairo_close_path(cr);
			cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.30);
			cairo_fill_preserve(cr);
			cairo_set_line_width(cr, 1.2);
			cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.9);
			cairo_stroke(cr);
		}
		return;
	}
	double radius = radius_km(level) / 111.0 * view.px_per_deg;
	if (radius >= 5) {
		cairo_arc(cr, x, y, radius, 0, 2 * M_PI);
		cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.22);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 1.5);
		cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.9);
		cairo_stroke(cr);
		return;
	}
	/* Nearer than the map can show: a pin, exactly there. */
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

/* A card that's only saying GeoClue went: its keys and closing answer
 * nothing (a new card may be showing). */
bool saying_gone(GtkWidget *w) {
	return g_object_get_data(G_OBJECT(w), "wb-gone") != nullptr;
}

gboolean on_key(GtkEventControllerKey *c, guint keyval, guint, GdkModifierType, gpointer) {
	if (saying_gone(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c))))
		return FALSE;
	if (keyval == GDK_KEY_Escape) {
		answer(false);  /* Escape: Deny; Return decides nothing */
		return TRUE;
	}
	return FALSE;
}

/* A gentle entrance: a short fade in (none with animations off). */
gboolean fade_in(GtkWidget *w, GdkFrameClock *clock, gpointer start_p) {
	gint64 start = GPOINTER_TO_SIZE(start_p);
	double t = (gdk_frame_clock_get_frame_time(clock) - start) / 160000.0;
	gtk_widget_set_opacity(w, std::min(1.0, t));
	return t < 1.0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void show_card(const Request &r) {
	AppLook look = app_look(r.app);
	GtkWidget *win = gtk_application_window_new(g.app);
	g.card = GTK_WINDOW(win);
	gtk_window_set_title(g.card, "Location request");
	gtk_window_set_decorated(g.card, FALSE);
	gtk_window_set_resizable(g.card, FALSE);
	gtk_widget_add_css_class(win, "wb-location");

	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
	gtk_widget_add_css_class(card, "wb-location-card");
	gtk_widget_set_size_request(card, 420, -1);

	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	GtkWidget *icon = gtk_image_new_from_gicon(look.icon);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(head), icon);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	char *esc = g_markup_escape_text(look.name.c_str(), -1);
	std::string title = std::string("<b>") + esc + "</b> wants to know where you are";
	g_free(esc);
	GtkWidget *t = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(t), title.c_str());
	gtk_label_set_wrap(GTK_LABEL(t), TRUE);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_widget_add_css_class(t, "wb-location-title");
	gtk_box_append(GTK_BOX(text), t);
	std::string near = "If you allow it, it learns where you are " + loc::nearness(r.level) + ".";
	GtkWidget *n = gtk_label_new(near.c_str());
	gtk_label_set_wrap(GTK_LABEL(n), TRUE);
	gtk_label_set_xalign(GTK_LABEL(n), 0);
	gtk_widget_add_css_class(n, "wb-location-near");
	gtk_box_append(GTK_BOX(text), n);
	if (!look.known) {
		/* No desktop file: the title has what GeoClue called it. */
		std::string note = "No installed app has this name.";
		GtkWidget *nn = gtk_label_new(note.c_str());
		gtk_label_set_xalign(GTK_LABEL(nn), 0);
		gtk_widget_add_css_class(nn, "wb-location-note");
		gtk_box_append(GTK_BOX(text), nn);
	}
	gtk_widget_set_hexpand(text, TRUE);
	gtk_box_append(GTK_BOX(head), text);
	gtk_box_append(GTK_BOX(card), head);

	/* How near, on a map around the time zone's city (none known: no map). */
	if (const wbconf::GeoData &geo = wbconf::geo_data(); geo.zone_at) {
		GtkWidget *map = gtk_drawing_area_new();
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(map), 120);
		gtk_widget_set_hexpand(map, TRUE);
		gtk_widget_add_css_class(map, "wb-location-map");
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(map), draw_map,
				GUINT_TO_POINTER(r.level), nullptr);
		gtk_box_append(GTK_BOX(card), map);
		std::string where = "Around " + geo.zone_city +
				", your time zone's city. Nothing is looked up before you answer.";
		GtkWidget *cap = gtk_label_new(where.c_str());
		gtk_label_set_wrap(GTK_LABEL(cap), TRUE);
		gtk_label_set_xalign(GTK_LABEL(cap), 0);
		gtk_widget_add_css_class(cap, "wb-location-note");
		gtk_widget_set_margin_top(cap, -8);
		gtk_box_append(GTK_BOX(card), cap);
	}

	/* How long the answer holds; then the buttons. */
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	const char *holds[] = {"Just this time", "For this session", "Always", nullptr};
	g.hold = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(holds));
	gtk_widget_set_tooltip_text(GTK_WIDGET(g.hold), "How long your answer holds, either way");
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(g.hold));
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(row), spacer);
	/* Alike: neither answer is the one the card leans to. */
	GtkWidget *deny = gtk_button_new_with_mnemonic("_Deny");
	GtkWidget *allow = gtk_button_new_with_mnemonic("_Allow");
	g_signal_connect(deny, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { answer(false); }),
			nullptr);
	g_signal_connect(allow, "clicked", G_CALLBACK(+[](GtkButton *, gpointer) { answer(true); }),
			nullptr);
	gtk_box_append(GTK_BOX(row), deny);
	gtk_box_append(GTK_BOX(row), allow);
	gtk_box_append(GTK_BOX(card), row);

	g.every = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
			"Answer every app the same way until I log out"));
	gtk_widget_add_css_class(GTK_WIDGET(g.every), "wb-location-note");
	gtk_box_append(GTK_BOX(card), GTK_WIDGET(g.every));

	gtk_window_set_child(g.card, card);
	/* No default: a stray Return decides nothing; Escape denies. */
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), nullptr);
	gtk_widget_add_controller(win, keys);
	g_signal_connect(win, "close-request",
			G_CALLBACK(+[](GtkWindow *self, gpointer) -> gboolean {
				if (saying_gone(GTK_WIDGET(self)))
					return FALSE;
				answer(false);  /* closed: as Deny, this time */
				return TRUE;
			}),
			nullptr);
	gboolean animate = TRUE;
	g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &animate, nullptr);
	if (animate) {
		gtk_widget_set_opacity(win, 0);
		gtk_widget_add_tick_callback(win, fade_in,
				GSIZE_TO_POINTER(g_get_monotonic_time()), nullptr);
	}
	gtk_window_present(g.card);
	g_object_unref(look.icon);

	g_printerr("card: %s wants to know where you are|%s|%s\n", look.name.c_str(), near.c_str(),
			look.known ? "" : r.app.c_str());
	if (const char *test = getenv("WAYBOX_LOCATION_TEST_ANSWER")) {
		std::string spec = test;
		if (!spec.empty() && spec[0] == '@') {  /* read from a file, each card */
			gchar *text = nullptr;
			g_file_get_contents(spec.c_str() + 1, &text, nullptr, nullptr);
			spec = text != nullptr ? g_strstrip(text) : "";
			g_free(text);
		}
		if (spec == "wait")
			return;
		bool yes = spec.rfind("allow", 0) == 0;
		guint hold = spec.find("always") != std::string::npos ? 2
				: spec.find("session") != std::string::npos ? 1 : 0;
		gtk_drop_down_set_selected(g.hold, hold);
		if (spec.find("every") != std::string::npos)
			gtk_check_button_set_active(g.every, TRUE);
		g_timeout_add_once(300, [](gpointer y) { answer(GPOINTER_TO_INT(y) != 0); },
				GINT_TO_POINTER(yes ? 1 : 0));
	}
}

/* The first asked, with no answer kept: its card; those with one,
 * answered at once. */
void show_next() {
	while (!g.queue.empty() && g.card == nullptr) {
		Request &r = g.queue.front();
		if (std::optional<loc::Answer> a = kept(r)) {
			Request done = r;
			g.queue.pop_front();
			reply(done, *a);
			continue;
		}
		show_card(r);
	}
}

/* ---- GeoClue ---- */

void on_method(GDBusConnection *, const char *, const char *, const char *, const char *method,
		GVariant *params, GDBusMethodInvocation *invocation, gpointer) {
	if (g_strcmp0(method, "AuthorizeApp") != 0) {
		g_dbus_method_invocation_return_dbus_error(invocation,
				"org.freedesktop.DBus.Error.UnknownMethod", "no such method");
		return;
	}
	const char *id = nullptr;
	guint32 level = 0;
	g_variant_get(params, "(&su)", &id, &level);
	g_printerr("waybox-location-agent: %s asks (%s)\n", id, loc::level_name(level).c_str());
	g.queue.push_back({id, level, invocation});
	show_next();
}

GVariant *on_get(GDBusConnection *, const char *, const char *, const char *, const char *prop,
		GError **, gpointer) {
	if (g_strcmp0(prop, "MaxAccuracyLevel") == 0)
		return g_variant_new_uint32(loc::kExact);
	return nullptr;
}

/* GeoClue went (stopped, restarted): no one waits for the answers
 * asked; the card says so, then goes. */
void geoclue_gone() {
	std::string name;
	if (!g.queue.empty())
		name = app_look(g.queue.front().app).name;
	for (Request &r : g.queue)
		if (r.invocation != nullptr)
			g_dbus_method_invocation_return_dbus_error(r.invocation,
					"org.freedesktop.DBus.Error.NoReply", "GeoClue stopped");
	g.queue.clear();
	if (g.card == nullptr)
		return;
	GtkWindow *win = g.card;
	g.card = nullptr;
	g_object_set_data(G_OBJECT(win), "wb-gone", GINT_TO_POINTER(1));
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(card, "wb-location-card");
	gtk_widget_set_size_request(card, 420, -1);
	GtkWidget *t = gtk_label_new("The location service stopped");
	gtk_widget_add_css_class(t, "wb-location-title");
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_box_append(GTK_BOX(card), t);
	std::string why = name + " no longer waits for an answer: it learns nothing.";
	GtkWidget *n = gtk_label_new(why.c_str());
	gtk_label_set_wrap(GTK_LABEL(n), TRUE);
	gtk_label_set_xalign(GTK_LABEL(n), 0);
	gtk_widget_add_css_class(n, "wb-location-near");
	gtk_box_append(GTK_BOX(card), n);
	gtk_window_set_child(win, card);
	g_printerr("card: closed, GeoClue stopped|%s\n", why.c_str());
	g_timeout_add_seconds_once(4, [](gpointer w) { gtk_window_destroy(GTK_WINDOW(w)); }, win);
}

/* AddAgent with our own ID; refused (not on GeoClue's list), with the
 * demo agent's, which every distribution's list has. */
bool add_agent() {
	for (const char *id : {kAppId, kFallbackId}) {
		GError *err = nullptr;
		GVariant *r = g_dbus_connection_call_sync(g.system, "org.freedesktop.GeoClue2",
				"/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager",
				"AddAgent", g_variant_new("(s)", id), nullptr, G_DBUS_CALL_FLAGS_NONE, 10000,
				nullptr, &err);
		if (r != nullptr) {
			g_variant_unref(r);
			g.registered_as = id;
			g_printerr("waybox-location-agent: registered as %s%s\n", id,
					id == kFallbackId ? " (GeoClue doesn't list our own ID)" : "");
			return true;
		}
		bool missing = g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
				g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
		g_printerr("waybox-location-agent: AddAgent(%s): %s\n", id, err->message);
		g_error_free(err);
		if (missing)
			return false;  /* no GeoClue (yet): watched for */
	}
	return false;
}

void activate(GtkApplication *app, gpointer) {
	if (g.system != nullptr)
		return;  /* (started again: one agent) */
	g_application_hold(G_APPLICATION(app));
	GtkCssProvider *css = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_css_provider_load_from_string(css, kCss);
#else
	gtk_css_provider_load_from_data(css, kCss, -1);
#endif
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	GError *err = nullptr;
	g.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
	if (g.system == nullptr) {
		g_printerr("waybox-location-agent: no system bus: %s\n", err->message);
		g_error_free(err);
		g_application_release(G_APPLICATION(app));
		return;
	}
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kAgentXml, nullptr);
	static const GDBusInterfaceVTable vtable = {on_method, on_get, nullptr, {nullptr}};
	g_dbus_connection_register_object(g.system, "/org/freedesktop/GeoClue2/Agent",
			node->interfaces[0], &vtable, nullptr, nullptr, nullptr);
	g_dbus_node_info_unref(node);
	/* Registered when GeoClue is there (started, where it's activatable),
	 * and again whenever it restarts. */
	g.watch = g_bus_watch_name_on_connection(g.system, "org.freedesktop.GeoClue2",
			G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
			[](GDBusConnection *, const char *, const char *, gpointer) { add_agent(); },
			[](GDBusConnection *, const char *, gpointer) { geoclue_gone(); }, nullptr,
			nullptr);
}

}  // namespace

int main(int argc, char **argv) {
	g.app = gtk_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(g.app, "activate", G_CALLBACK(activate), nullptr);
	int status = g_application_run(G_APPLICATION(g.app), argc, argv);
	g_object_unref(g.app);
	return status;
}
