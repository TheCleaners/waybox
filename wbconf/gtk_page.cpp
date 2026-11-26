/*
 * Appearance: Applications (GTK). The icon, GTK and cursor themes GTK
 * programs use, and their light/dark preference. Stored in GSettings
 * (org.gnome.desktop.interface, what GTK 4 and the settings portal read)
 * and in the GTK 3 and 4 settings.ini files that exist, so they agree.
 * These are shared with other desktops on the same account.
 *
 * And a check: an icon theme whose fallback (Inherits=) isn't installed
 * shows GTK's missing-icon placeholder for whatever it lacks; wbconf says
 * so and can fill the gap with a stand-in falling back to a complete
 * theme (#192).
 */
#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

const char *kSchema = "org.gnome.desktop.interface";

bool have_interface_schema() {
	GSettingsSchemaSource *src = g_settings_schema_source_get_default();
	if (src == nullptr)
		return false;
	GSettingsSchema *schema = g_settings_schema_source_lookup(src, kSchema, TRUE);
	if (schema == nullptr)
		return false;
	g_settings_schema_unref(schema);
	return true;
}

GSettings *interface_settings() {
	return have_interface_schema() ? g_settings_new(kSchema) : nullptr;
}

/* GLib aborts on a key the schema doesn't have (color-scheme only came
 * with GNOME 42's schemas). */
bool has_key(GSettings *s, const char *key) {
	GSettingsSchema *schema = nullptr;
	g_object_get(s, "settings-schema", &schema, nullptr);
	bool ok = schema != nullptr && g_settings_schema_has_key(schema, key);
	if (schema != nullptr)
		g_settings_schema_unref(schema);
	return ok;
}

std::string data_home() {
	const char *x = getenv("XDG_DATA_HOME");
	return x && *x ? x : home_dir() + "/.local/share";
}

std::string config_home() {
	const char *x = getenv("XDG_CONFIG_HOME");
	return x && *x ? x : home_dir() + "/.config";
}

std::vector<std::string> icon_dirs() {
	const char *dd = getenv("XDG_DATA_DIRS");
	return wb::icon_theme_dirs(home_dir(), data_home(), dd ? dd : "");
}

/* A key of [Settings] in a GTK settings.ini ("" if absent). */
std::string ini_get(const std::string &path, const std::string &key) {
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line)) {
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = line.substr(0, eq);
		while (!k.empty() && k.back() == ' ')
			k.pop_back();
		if (k == key) {
			std::string v = line.substr(eq + 1);
			while (!v.empty() && v.front() == ' ')
				v.erase(0, 1);
			return v;
		}
	}
	return "";
}

/* The file with those keys set ("" value: removed), the rest kept. */
std::string ini_set(const std::string &text, const std::map<std::string, std::string> &values) {
	std::vector<std::string> lines;
	std::stringstream in(text);
	std::string line;
	std::set<std::string> done;
	bool in_settings = false;
	int settings_end = -1;
	while (std::getline(in, line)) {
		if (!line.empty() && line[0] == '[') {
			if (in_settings)
				settings_end = static_cast<int>(lines.size());
			in_settings = line == "[Settings]";
		}
		size_t eq = line.find('=');
		if (in_settings && eq != std::string::npos) {
			std::string k = line.substr(0, eq);
			while (!k.empty() && k.back() == ' ')
				k.pop_back();
			auto v = values.find(k);
			if (v != values.end()) {
				done.insert(k);
				if (!v->second.empty())
					lines.push_back(k + "=" + v->second);
				continue;
			}
		}
		lines.push_back(line);
	}
	if (in_settings)
		settings_end = static_cast<int>(lines.size());
	if (settings_end < 0) {
		lines.push_back("[Settings]");
		settings_end = static_cast<int>(lines.size());
	}
	std::vector<std::string> add;
	for (const auto &[k, v] : values)
		if (done.count(k) == 0 && !v.empty())
			add.push_back(k + "=" + v);
	lines.insert(lines.begin() + settings_end, add.begin(), add.end());
	std::string out;
	for (const std::string &l : lines)
		out += l + "\n";
	return out;
}

Ui::GtkLook read_look() {
	Ui::GtkLook l;
	if (GSettings *s = interface_settings()) {
		auto str = [s](const char *key) {
			if (!has_key(s, key))
				return std::string();
			gchar *v = g_settings_get_string(s, key);
			std::string out = v ? v : "";
			g_free(v);
			return out;
		};
		l.icons = str("icon-theme");
		l.theme = str("gtk-theme");
		l.cursor = str("cursor-theme");
		l.size = has_key(s, "cursor-size") ? g_settings_get_int(s, "cursor-size") : 24;
		l.scheme = str("color-scheme");
		g_object_unref(s);
		return l;
	}
	std::string ini = config_home() + "/gtk-4.0/settings.ini";
	l.icons = ini_get(ini, "gtk-icon-theme-name");
	l.theme = ini_get(ini, "gtk-theme-name");
	l.cursor = ini_get(ini, "gtk-cursor-theme-name");
	std::string size = ini_get(ini, "gtk-cursor-theme-size");
	l.size = size.empty() ? 24 : atoi(size.c_str());
	std::string dark = ini_get(ini, "gtk-application-prefer-dark-theme");
	l.scheme = dark == "true" || dark == "1" ? "prefer-dark" : "default";
	return l;
}

Ui::GtkLook current_look(Ui *ui) {
	Ui::GtkLook l = ui->gtk.saved;
	if (!ui->gtk.built)
		return l;
	l.icons = selected_value(ui->gtk.icons, ui->gtk.icon_values);
	l.theme = selected_value(ui->gtk.theme, ui->gtk.theme_values);
	l.cursor = selected_value(ui->gtk.cursor, ui->gtk.cursor_values);
	l.scheme = selected_value(ui->gtk.scheme, ui->gtk.scheme_values);
	l.size = gtk_spin_button_get_value_as_int(ui->gtk.size);
	return l;
}

/* The stand-ins wbconf made in the user's icons folder for `id`'s chain. */
std::vector<std::string> stand_ins_for(Ui *ui, const std::string &id) {
	std::vector<std::string> out;
	std::set<std::string> seen;
	std::vector<std::string> queue{id};
	for (size_t i = 0; i < queue.size(); ++i) {
		if (!seen.insert(queue[i]).second)
			continue;
		auto t = ui->gtk.icon_themes.find(queue[i]);
		if (t == ui->gtk.icon_themes.end())
			continue;
		if (t->second.stand_in)
			out.push_back(t->first);
		for (const std::string &p : t->second.inherits)
			queue.push_back(p);
	}
	return out;
}

void refresh_health(Ui *ui) {
	if (!ui->gtk.built)
		return;
	std::string id = selected_value(ui->gtk.icons, ui->gtk.icon_values);
	std::vector<std::string> missing = wb::missing_fallbacks(id, ui->gtk.icon_themes);
	missing.erase(std::remove(missing.begin(), missing.end(), id), missing.end());
	std::string target = wb::complete_icon_theme(ui->gtk.icon_themes);
	std::vector<std::string> made = stand_ins_for(ui, id);
	std::string text;
	bool warn = false;
	if (!missing.empty()) {
		std::string list;
		for (const std::string &m : missing)
			list += (list.empty() ? "" : ", ") + m;
		char *t = g_strdup_printf(_("%s falls back to %s, which isn't installed: icons it "
				"doesn't have show as missing in GTK programs."), id.c_str(), list.c_str());
		text = t;
		g_free(t);
		warn = true;
	} else if (!made.empty()) {
		text = _("Icons this theme doesn't have come from a stand-in wbconf made.");
	}
	gtk_label_set_text(ui->gtk.health, text.c_str());
	gtk_widget_set_visible(GTK_WIDGET(ui->gtk.health), !text.empty());
	if (warn)
		gtk_widget_add_css_class(GTK_WIDGET(ui->gtk.health), "warning");
	else
		gtk_widget_remove_css_class(GTK_WIDGET(ui->gtk.health), "warning");
	bool can_fill = !missing.empty() && !target.empty();
	if (can_fill) {
		char *label = g_strdup_printf(_("Fill the Gaps from %s"), target.c_str());
		gtk_button_set_label(GTK_BUTTON(ui->gtk.fill), label);
		g_free(label);
	}
	gtk_widget_set_visible(ui->gtk.fill, can_fill);
	gtk_widget_set_visible(ui->gtk.undo, missing.empty() && !made.empty());
}

void rescan_icons(Ui *ui) {
	ui->gtk.icon_themes = wb::find_icon_themes(icon_dirs());
	/* A stand-in wbconf made, now hiding a real theme of that name that
	 * was installed since: it goes (only ever its own, marked files in the
	 * user's folder). The session script does the same at login. */
	bool removed = false;
	for (const auto &[id, t] : ui->gtk.icon_themes) {
		if (!t.stand_in || !t.hides_real || t.path.rfind(data_home() + "/icons/", 0) != 0)
			continue;
		std::error_code ec;
		fs::remove(fs::path(t.path) / "index.theme", ec);
		fs::remove(t.path, ec);
		removed = true;
	}
	if (removed)
		ui->gtk.icon_themes = wb::find_icon_themes(icon_dirs());
}

/* Write a stand-in for each missing fallback of the selected theme. */
void fill_gaps(Ui *ui) {
	std::string id = selected_value(ui->gtk.icons, ui->gtk.icon_values);
	std::string target = wb::complete_icon_theme(ui->gtk.icon_themes);
	if (target.empty())
		return;
	std::string err;
	for (const std::string &m : wb::missing_fallbacks(id, ui->gtk.icon_themes)) {
		if (m == id || m == "hicolor" || m.find('/') != std::string::npos || m == "..")
			continue;
		fs::path dir = fs::path(data_home()) / "icons" / m;
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::ofstream f(dir / "index.theme", std::ios::trunc);
		f << wb::stand_in_theme(m, target);
		if (!f)
			err = dir.string();
	}
	rescan_icons(ui);
	refresh_health(ui);
	set_status(ui, err.empty()
			? _("Gaps filled: GTK programs show the missing icons once restarted.")
			: std::string(_("Could not write ")) + err);
}

void on_fill(GtkButton *, gpointer data) {
	fill_gaps(static_cast<Ui *>(data));
}

void on_undo(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string id = selected_value(ui->gtk.icons, ui->gtk.icon_values);
	for (const std::string &m : stand_ins_for(ui, id)) {
		const wb::IconTheme &t = ui->gtk.icon_themes.at(m);
		/* Only ever its own: marked, and in the user's folder. */
		if (t.stand_in && t.path.rfind(data_home() + "/icons/", 0) == 0) {
			std::error_code ec;
			fs::remove(fs::path(t.path) / "index.theme", ec);
			fs::remove(t.path, ec);
		}
	}
	rescan_icons(ui);
	refresh_health(ui);
	set_status(ui, _("Stand-in removed."));
}

void on_icons_changed(GObject *, GParamSpec *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	refresh_health(ui);
	refresh_save_state(ui);
}

void on_look_changed(GObject *, GParamSpec *, gpointer data) {
	refresh_save_state(static_cast<Ui *>(data));
}

GtkDropDown *names_dropdown(std::vector<std::string> names, std::vector<std::string> &values,
		const std::string &current) {
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return g_ascii_strcasecmp(a.c_str(), b.c_str()) < 0;
	});
	std::vector<std::pair<std::string, std::string>> items;
	for (const std::string &n : names)
		items.emplace_back(n, n);
	GtkDropDown *dd = choice_dropdown(items, values);
	if (!current.empty())
		select_value(dd, values, current);
	return dd;
}

}  // namespace

wb::SettingDef gtk_appearance_setting() {
	wb::SettingDef d;
	d.id = "gtk.appearance";
	d.section = _("Applications (GTK)");
	d.title = _("GTK applications");
	d.description = _("The themes GTK programs use. Other desktops on this account share "
			"these settings.");
	d.keywords = "gtk icon theme cursor dark light color scheme adwaita gsettings";
	d.type = wb::SettingType::Custom;
	d.widget = "gtk-appearance";
	return d;
}

GtkWidget *build_gtk_block(Ui *ui) {
	ui->gtk.saved = read_look();
	rescan_icons(ui);
	std::vector<std::string> icon_names, cursor_names;
	for (const auto &[id, t] : ui->gtk.icon_themes) {
		if (t.has_icons && !t.hidden && !t.stand_in && id != "hicolor" && id != "default")
			icon_names.push_back(id);
		if (t.has_cursors)
			cursor_names.push_back(id);
	}
	const char *dd = getenv("XDG_DATA_DIRS");
	std::vector<std::string> gtk_names = wb::find_gtk_themes(
			wb::gtk_theme_dirs(home_dir(), data_home(), dd ? dd : ""));
	if (std::find(gtk_names.begin(), gtk_names.end(), "Adwaita") == gtk_names.end())
		gtk_names.push_back("Adwaita");  /* built into GTK */

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	int row = 0;
	grid_label(grid, row, _("Icons"));
	ui->gtk.icons = names_dropdown(icon_names, ui->gtk.icon_values, ui->gtk.saved.icons);
	grid_control(grid, row++, GTK_WIDGET(ui->gtk.icons));
	GtkWidget *health = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(health), TRUE);
	gtk_label_set_xalign(GTK_LABEL(health), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(health), 60);
	ui->gtk.health = GTK_LABEL(health);
	gtk_grid_attach(GTK_GRID(grid), health, 1, row++, 1, 1);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	ui->gtk.fill = gtk_button_new_with_label("");
	ui->gtk.undo = gtk_button_new_with_label(_("Remove the Stand-In"));
	g_signal_connect(ui->gtk.fill, "clicked", G_CALLBACK(on_fill), ui);
	g_signal_connect(ui->gtk.undo, "clicked", G_CALLBACK(on_undo), ui);
	gtk_box_append(GTK_BOX(buttons), ui->gtk.fill);
	gtk_box_append(GTK_BOX(buttons), ui->gtk.undo);
	gtk_grid_attach(GTK_GRID(grid), buttons, 1, row++, 1, 1);

	grid_label(grid, row, _("GTK theme"));
	ui->gtk.theme = names_dropdown(gtk_names, ui->gtk.theme_values, ui->gtk.saved.theme);
	grid_control(grid, row++, GTK_WIDGET(ui->gtk.theme));
	/* The pointer is Waybox's to draw for most programs (cursor-shape,
	 * X11): rc.xml's <cursor> wins where it's set, and saving writes both. */
	if (ui->gtk.rc_cursor_theme && !ui->gtk.rc_cursor_theme->empty())
		ui->gtk.saved.cursor = *ui->gtk.rc_cursor_theme;
	if (ui->gtk.rc_cursor_size && atoi(ui->gtk.rc_cursor_size->c_str()) >= 8)
		ui->gtk.saved.size = atoi(ui->gtk.rc_cursor_size->c_str());
	grid_label(grid, row, _("Pointer"));
	ui->gtk.cursor = names_dropdown(cursor_names, ui->gtk.cursor_values, ui->gtk.saved.cursor);
	gtk_widget_set_tooltip_text(GTK_WIDGET(ui->gtk.cursor), _("For every program: Waybox's "
			"pointer (rc.xml <cursor>) and GTK's own setting"));
	grid_control(grid, row++, GTK_WIDGET(ui->gtk.cursor));
	ui->gtk.size = add_spin(grid, row++, _("Pointer size"), 8, 256);
	gtk_widget_set_tooltip_text(GTK_WIDGET(ui->gtk.size), _("For every program: Waybox's "
			"pointer (rc.xml <cursor>) and GTK's own setting"));
	gtk_spin_button_set_value(ui->gtk.size, ui->gtk.saved.size);
	grid_label(grid, row, _("Style"));
	ui->gtk.scheme = choice_dropdown({{"default", _("As the program prefers")},
			{"prefer-dark", _("Dark")}, {"prefer-light", _("Light")}}, ui->gtk.scheme_values);
	select_value(ui->gtk.scheme, ui->gtk.scheme_values,
			ui->gtk.saved.scheme.empty() ? "default" : ui->gtk.saved.scheme);
	grid_control(grid, row++, GTK_WIDGET(ui->gtk.scheme));

	/* What the controls show now counts as saved (a value not installed
	 * was added to its list, so it shows as it is). */
	ui->gtk.built = true;
	ui->gtk.saved = current_look(ui);
	g_signal_connect(ui->gtk.icons, "notify::selected", G_CALLBACK(on_icons_changed), ui);
	for (GtkDropDown *d : {ui->gtk.theme, ui->gtk.cursor, ui->gtk.scheme})
		g_signal_connect(d, "notify::selected", G_CALLBACK(on_look_changed), ui);
	g_signal_connect(ui->gtk.size, "notify::value", G_CALLBACK(on_look_changed), ui);
	refresh_health(ui);
	return grid;
}

void gtk_changes(Ui *ui, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out) {
	if (!ui->gtk.built)
		return;
	Ui::GtkLook was = ui->gtk.saved, now = current_look(ui);
	auto one = [&](const char *what, const std::string &a, const std::string &b) {
		if (a != b)
			out.push_back({{where + " \xE2\x80\xBA " + what, a, b}, target});
	};
	one(_("Icons"), was.icons, now.icons);
	one(_("GTK theme"), was.theme, now.theme);
	one(_("Pointer"), was.cursor, now.cursor);
	one(_("Pointer size"), std::to_string(was.size), std::to_string(now.size));
	one(_("Style"), was.scheme, now.scheme);
}

int gtk_unsaved(Ui *ui) {
	if (!ui->gtk.built)
		return 0;
	return current_look(ui) == ui->gtk.saved ? 0 : 1;
}

bool save_gtk(Ui *ui, std::string *error) {
	if (gtk_unsaved(ui) == 0)
		return true;
	Ui::GtkLook l = current_look(ui);
	if (GSettings *s = interface_settings()) {
		auto set = [s](const char *key, const std::string &v) {
			if (has_key(s, key))
				g_settings_set_string(s, key, v.c_str());
		};
		set("icon-theme", l.icons);
		set("gtk-theme", l.theme);
		set("cursor-theme", l.cursor);
		if (has_key(s, "cursor-size"))
			g_settings_set_int(s, "cursor-size", l.size);
		set("color-scheme", l.scheme);
		g_settings_sync();
		g_object_unref(s);
	}
	/* settings.ini: the ones that exist (else, with no GSettings, GTK 4's). */
	std::map<std::string, std::string> keys = {
		{"gtk-icon-theme-name", l.icons}, {"gtk-theme-name", l.theme},
		{"gtk-cursor-theme-name", l.cursor},
		{"gtk-cursor-theme-size", std::to_string(l.size)},
		{"gtk-application-prefer-dark-theme", l.scheme == "prefer-dark" ? "true"
				: l.scheme == "prefer-light" ? "false" : ""},
	};
	bool have_gsettings = have_interface_schema();
	for (const char *v : {"gtk-3.0", "gtk-4.0"}) {
		fs::path p = fs::path(config_home()) / v / "settings.ini";
		std::error_code ec;
		bool exists = fs::exists(p, ec);
		if (!exists && (have_gsettings || std::string(v) != "gtk-4.0"))
			continue;
		std::ifstream in(p);
		std::stringstream ss;
		ss << in.rdbuf();
		fs::create_directories(p.parent_path(), ec);
		if (!write_file_atomic(p.string(), ini_set(ss.str(), keys), error))
			return false;
	}
	ui->gtk.saved = l;
	return true;
}

void gtk_test_value(Ui *ui, const std::string &key, const std::string &value) {
	/* "gtk.icons=ID", "gtk.fill=1", "gtk.scheme=prefer-dark", "gtk.size=40". */
	if (!ui->gtk.built)
		return;
	if (key == "gtk.icons")
		select_value(ui->gtk.icons, ui->gtk.icon_values, value);
	else if (key == "gtk.scheme")
		select_value(ui->gtk.scheme, ui->gtk.scheme_values, value);
	else if (key == "gtk.fill")
		fill_gaps(ui);
	else if (key == "gtk.size")
		gtk_spin_button_set_value(ui->gtk.size, atoi(value.c_str()));
}

}  // namespace wbconf
