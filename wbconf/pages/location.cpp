/*
 * Page=location: the location agent's remembered answers (#363). Answers
 * kept always are in the xdg permission store's "location" table, where
 * xdg-desktop-portal keeps its own for sandboxed apps: listed here (the
 * app, allowed how near or denied, when), each forgotten on its own or all
 * at once. Forgetting acts at once (the store is the app's next answer,
 * not a setting saved later); the list follows the store as it changes.
 * Answers for the session only live in the running agent and go at logout.
 */
#include "../wbconf_ui.hpp"

#include <gio/gdesktopappinfo.h>

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "waybox/location_agent.hpp"

namespace wbconf {

namespace {

namespace loc = wb::location;

constexpr const char *kStore = "org.freedesktop.impl.portal.PermissionStore";
constexpr const char *kStorePath = "/org/freedesktop/impl/portal/PermissionStore";

struct Page {
	GDBusConnection *bus = nullptr;
	GtkWidget *box = nullptr, *list = nullptr, *note = nullptr, *forget_all = nullptr;
	guint changed = 0;
	std::map<std::string, std::vector<std::string>> entries;
	bool reachable = false;
	~Page() {
		if (changed != 0)
			g_dbus_connection_signal_unsubscribe(bus, changed);
		if (bus != nullptr)
			g_object_unref(bus);
	}
};

/* The page showing now (one wbconf window): for the tests' steps. */
Page *current = nullptr;

/* The table's entries; false if the store can't be reached. */
bool lookup(Page *p) {
	p->entries.clear();
	if (p->bus == nullptr)
		return false;
	GError *err = nullptr;
	GVariant *r = g_dbus_connection_call_sync(p->bus, kStore, kStorePath, kStore, "Lookup",
			g_variant_new("(ss)", "location", "location"), G_VARIANT_TYPE("(a{sas}v)"),
			G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);
	if (r == nullptr) {
		/* Never asked yet: no table, no entry; that's an empty list. */
		bool none = g_dbus_error_is_remote_error(err) &&
				g_str_has_suffix(g_dbus_error_get_remote_error(err), ".NotFound");
		g_error_free(err);
		return none;
	}
	GVariantIter *it = nullptr;
	g_variant_get(r, "(a{sas}v)", &it, nullptr);
	const char *app = nullptr;
	GVariantIter *vals = nullptr;
	while (g_variant_iter_next(it, "{&sas}", &app, &vals)) {
		std::vector<std::string> v;
		const char *s = nullptr;
		while (g_variant_iter_next(vals, "&s", &s))
			v.push_back(s);
		g_variant_iter_free(vals);
		p->entries[app] = v;
	}
	g_variant_iter_free(it);
	g_variant_unref(r);
	return true;
}

/* "today", "yesterday", "3 days ago", else the date. */
std::string when(const std::string &seconds) {
	gint64 t = g_ascii_strtoll(seconds.c_str(), nullptr, 10);
	if (t <= 0)
		return "";
	gint64 days = (static_cast<gint64>(time(nullptr)) - t) / 86400;
	if (days <= 0)
		return _("today");
	if (days == 1)
		return _("yesterday");
	if (days < 30) {
		char *s = g_strdup_printf(_("%d days ago"), static_cast<int>(days));
		std::string out = s;
		g_free(s);
		return out;
	}
	GDateTime *d = g_date_time_new_from_unix_local(t);
	char *s = g_date_time_format(d, "%-d %b %Y");
	std::string out = s;
	g_free(s);
	g_date_time_unref(d);
	return out;
}

/* "Allowed, to within a city · answered today" / "Denied · ..." */
std::string describe(const std::vector<std::string> &v) {
	std::string what;
	auto level = v.empty() ? std::nullopt : loc::level_from_name(v[0]);
	if (!level)
		what = _("Kept by another program, not read here");
	else if (*level == loc::kNone)
		what = _("Denied");
	else
		what = std::string(_("Allowed")) + ", " + loc::nearness(*level);
	std::string w = v.size() > 1 ? when(v[1]) : "";
	return w.empty() ? what : what + " · " + _("answered") + " " + w;
}

void forget(Page *p, const std::string &app) {
	g_dbus_connection_call(p->bus, kStore, kStorePath, kStore, "DeletePermission",
			g_variant_new("(sss)", "location", "location", app.c_str()), nullptr,
			G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr, nullptr);
}

void forget_all(Page *p) {
	g_dbus_connection_call(p->bus, kStore, kStorePath, kStore, "Delete",
			g_variant_new("(ss)", "location", "location"), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000,
			nullptr, nullptr, nullptr);
}

void fill(Page *p) {
	while (GtkWidget *c = gtk_widget_get_first_child(p->list))
		gtk_box_remove(GTK_BOX(p->list), c);
	p->reachable = lookup(p);
	gtk_widget_set_sensitive(p->forget_all, p->reachable && !p->entries.empty());
	if (!p->reachable) {
		gtk_label_set_text(GTK_LABEL(p->note),
				_("The permission store (xdg-desktop-portal) isn't running: answers can't "
				  "be kept always, so every app is asked each time."));
		return;
	}
	gtk_label_set_text(GTK_LABEL(p->note), p->entries.empty()
			? _("None yet. An answer given for Always is remembered here, as are the ones "
			    "sandboxed apps get through the portal.")
			: _("Given at once, without asking. Answers for the session only are forgotten "
			    "at logout."));
	for (const auto &[app, v] : p->entries) {
		std::string name = app;
		GIcon *icon = nullptr;
		if (GDesktopAppInfo *info = g_desktop_app_info_new((app + ".desktop").c_str())) {
			name = g_app_info_get_display_name(G_APP_INFO(info));
			if (GIcon *i = g_app_info_get_icon(G_APP_INFO(info)))
				icon = G_ICON(g_object_ref(i));
			g_object_unref(info);
		}
		if (icon == nullptr)
			icon = g_themed_icon_new("find-location");
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		GtkWidget *img = gtk_image_new_from_gicon(icon);
		g_object_unref(icon);
		gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
		gtk_box_append(GTK_BOX(row), img);
		GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_hexpand(text, TRUE);
		gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
		GtkWidget *n = gtk_label_new(name.c_str());
		gtk_label_set_xalign(GTK_LABEL(n), 0);
		gtk_box_append(GTK_BOX(text), n);
		GtkWidget *d = gtk_label_new(describe(v).c_str());
		gtk_label_set_xalign(GTK_LABEL(d), 0);
		gtk_widget_add_css_class(d, "dim-label");
		gtk_box_append(GTK_BOX(text), d);
		gtk_box_append(GTK_BOX(row), text);
		GtkWidget *b = gtk_button_new_with_label(_("Forget"));
		gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(b, _("Asked again next time (at once, not on Save)"));
		g_object_set_data_full(G_OBJECT(b), "wb-app", g_strdup(app.c_str()), g_free);
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer data) {
			forget(static_cast<Page *>(data),
					static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "wb-app")));
		}), p);
		gtk_box_append(GTK_BOX(row), b);
		gtk_box_append(GTK_BOX(p->list), row);
	}
}

void on_changed(GDBusConnection *, const char *, const char *, const char *, const char *,
		GVariant *params, gpointer data) {
	const char *table = nullptr;
	g_variant_get_child(params, 0, "&s", &table);  /* (table, id, deleted, data, ...) */
	if (g_strcmp0(table, "location") == 0)
		fill(static_cast<Page *>(data));
}

}  // namespace

GtkWidget *build_location_page(Ui *, Ui::ProgramRow *) {
	auto *p = new Page;
	p->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_top(p->box, 6);
	gtk_widget_set_margin_bottom(p->box, 10);
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *title = gtk_label_new(_("Remembered answers"));
	gtk_widget_add_css_class(title, "heading");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_widget_set_hexpand(title, TRUE);
	gtk_box_append(GTK_BOX(head), title);
	p->forget_all = gtk_button_new_with_label(_("Forget All"));
	gtk_widget_set_tooltip_text(p->forget_all, _("Every app asked again (at once, not on Save)"));
	g_signal_connect(p->forget_all, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
		forget_all(static_cast<Page *>(data));
	}), p);
	gtk_box_append(GTK_BOX(head), p->forget_all);
	gtk_box_append(GTK_BOX(p->box), head);
	p->note = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->note), 0);
	gtk_label_set_wrap(GTK_LABEL(p->note), TRUE);
	gtk_widget_add_css_class(p->note, "dim-label");
	gtk_box_append(GTK_BOX(p->box), p->note);
	p->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_append(GTK_BOX(p->box), p->list);
	if (p->bus != nullptr)
		p->changed = g_dbus_connection_signal_subscribe(p->bus, kStore, kStore, "Changed",
				kStorePath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_changed, p, nullptr);
	fill(p);
	current = p;
	g_object_set_data_full(G_OBJECT(p->box), "wb-location-page", p, [](gpointer d) {
		if (current == d)
			current = nullptr;
		delete static_cast<Page *>(d);
	});
	return p->box;
}

std::string location_page_test(const std::string &step) {
	if (current == nullptr)
		return "none";
	if (step.rfind("forget:", 0) == 0)
		forget(current, step.substr(7));
	else if (step == "forget-all")
		forget_all(current);
	if (step != "dump")
		return "";
	std::string out = current->reachable ? "" : "unreachable";
	for (const auto &[app, v] : current->entries)
		out += "|" + app + "=" + describe(v);
	return out + "|note=" + gtk_label_get_text(GTK_LABEL(current->note));
}

}  // namespace wbconf
