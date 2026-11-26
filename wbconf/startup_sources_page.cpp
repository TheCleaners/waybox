/*
 * Programs that start from elsewhere than Waybox's own list: the autostart
 * script (its one-line program starts, switched by commenting them out) and
 * XDG autostart (switched with Hidden=true, written to the user's
 * directory). See #190.
 */
#include "wbconf_ui.hpp"

namespace wbconf {

/* Written to a copy next to it, then put in its place: a crash or a full
 * disk midway never leaves half a file. The file keeps its permissions
 * (a script stays executable). */
bool write_file_atomic(const std::string &path, const std::string &text, std::string *error) {
	std::string tmp = path + ".wbconf-new";
	{
		std::ofstream f(tmp, std::ios::trunc);
		f << text;
		f.close();
		if (!f) {
			*error = path + ": " + strerror(errno);
			std::error_code ec;
			fs::remove(tmp, ec);
			return false;
		}
	}
	std::error_code ec;
	auto st = fs::status(path, ec);
	if (!ec && fs::exists(st))
		fs::permissions(tmp, st.permissions(), ec);
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		*error = path + ": " + strerror(errno);
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

namespace {

std::string config_home() {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	return xdg && *xdg ? xdg : home_dir() + "/.config";
}

std::string read_file(const std::string &path) {
	std::ifstream f(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool write_file(const std::string &path, const std::string &text, std::string *error) {
	return write_file_atomic(path, text, error);
}

/* The desktops autostart entries are matched against, as xdg-autostart
 * does it. */
std::vector<std::string> current_desktops() {
	const char *env = getenv("WB_AUTOSTART_ENVIRONMENT");
	if (env == nullptr || *env == '\0')
		env = getenv("XDG_CURRENT_DESKTOP");
	std::vector<std::string> out;
	std::string cur;
	for (const char *p = env ? env : ""; ; ++p) {
		if (*p == ':' || *p == '\0') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
			if (*p == '\0')
				break;
		} else {
			cur += *p;
		}
	}
	return out;
}

std::vector<std::string> autostart_dirs() {
	std::vector<std::string> dirs{config_home() + "/autostart"};
	const char *cd = getenv("XDG_CONFIG_DIRS");
	std::string list = cd && *cd ? cd : "/etc/xdg";
	size_t start = 0;
	while (start <= list.size()) {
		size_t colon = list.find(':', start);
		std::string d = list.substr(start, colon == std::string::npos ? std::string::npos
				: colon - start);
		if (!d.empty())
			dirs.push_back(d + "/autostart");
		if (colon == std::string::npos)
			break;
		start = colon + 1;
	}
	return dirs;
}

void source_status(Ui *ui, Ui::SourceRow *r) {
	std::string text;
	auto add = [&text](const std::string &part) {
		text += (text.empty() ? "" : " · ") + part;
	};
	bool function = false;
	bool for_here = true;
	if (r->script) {
		for (const wb::ScriptEntry &e : ui->script_entries)
			if (e.line == r->line)
				function = e.function;
	} else {
		for (const wb::AutostartEntry &e : ui->xdg_entries)
			if (e.file == r->file)
				for_here = e.for_here;
	}
	if (function)
		add(_("a function in the script"));
	else if (is_running(ui, r->program))
		add(_("Running"));
	if (!for_here)
		add(_("not started here: limited to other desktops"));
	if (gtk_switch_get_active(r->on) != r->saved)
		add(gtk_switch_get_active(r->on) ? _("starts from the next session")
				: _("won't start from the next session"));
	gtk_label_set_text(r->status, text.c_str());
	gtk_widget_set_visible(GTK_WIDGET(r->status), !text.empty());
}

void on_source_toggled(GObject *, GParamSpec *, gpointer data) {
	auto *r = static_cast<Ui::SourceRow *>(data);
	auto *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(r->on), "wb-ui"));
	source_status(ui, r);
	refresh_save_state(ui);
}

/* A row: a switch, what runs, and its status below. */
GtkWidget *source_row_widget(Ui *ui, Ui::SourceRow *r, const std::string &title,
		const std::string &detail) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *sw = gtk_switch_new();
	gtk_widget_set_valign(sw, GTK_ALIGN_START);
	gtk_switch_set_active(GTK_SWITCH(sw), r->saved);
	r->on = GTK_SWITCH(sw);
	g_object_set_data(G_OBJECT(sw), "wb-ui", ui);
	gtk_box_append(GTK_BOX(row), sw);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(text, TRUE);
	GtkWidget *t = gtk_label_new(title.c_str());
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_box_append(GTK_BOX(text), t);
	if (!detail.empty()) {
		GtkWidget *d = gtk_label_new(detail.c_str());
		gtk_label_set_xalign(GTK_LABEL(d), 0);
		gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_END);
		gtk_label_set_selectable(GTK_LABEL(d), TRUE);
		gtk_widget_add_css_class(d, "monospace");
		gtk_widget_add_css_class(d, "dim-label");
		gtk_widget_set_tooltip_text(d, detail.c_str());
		gtk_box_append(GTK_BOX(text), d);
	}
	GtkWidget *status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status), 0);
	gtk_widget_add_css_class(status, "dim-label");
	gtk_widget_add_css_class(status, "caption");
	r->status = GTK_LABEL(status);
	gtk_box_append(GTK_BOX(text), status);
	gtk_box_append(GTK_BOX(row), text);
	source_status(ui, r);
	g_signal_connect(sw, "notify::active", G_CALLBACK(on_source_toggled), r);
	return row;
}

void on_edit_script(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	GFile *file = g_file_new_for_path(ui->script_path.c_str());
	GtkFileLauncher *launcher = gtk_file_launcher_new(file);
	gtk_file_launcher_launch(launcher, ui->window, nullptr, nullptr, nullptr);
	g_object_unref(launcher);
	g_object_unref(file);
}

}  // namespace

void load_startup_sources(Ui *ui) {
	std::string user = config_home() + "/waybox/autostart";
	std::string system = std::string(WB_SYSCONFDIR) + "/xdg/waybox/autostart";
	/* As waybox.sh: the user's if it's executable, else the default. */
	ui->script_is_user = access(user.c_str(), X_OK) == 0;
	ui->script_path = ui->script_is_user ? user
			: access(system.c_str(), R_OK) == 0 ? system : "";
	ui->script_entries = ui->script_path.empty() ? std::vector<wb::ScriptEntry>{}
			: wb::script_entries(read_file(ui->script_path));

	ui->xdg_entries.clear();
	std::set<std::string> seen;
	std::vector<std::string> desktops = current_desktops();
	std::vector<std::string> dirs = autostart_dirs();
	for (size_t i = 0; i < dirs.size(); ++i) {
		std::error_code ec;
		std::vector<std::string> names;
		for (const auto &de : fs::directory_iterator(dirs[i], ec))
			if (de.path().extension() == ".desktop")
				names.push_back(de.path().filename().string());
		std::sort(names.begin(), names.end());
		for (const std::string &name : names) {
			if (!seen.insert(name).second)
				continue;  /* shadowed by an earlier directory */
			wb::AutostartEntry e;
			if (!wb::parse_autostart(read_file(dirs[i] + "/" + name), desktops, e))
				continue;
			e.file = name;
			e.path = dirs[i] + "/" + name;
			e.user = i == 0;
			ui->xdg_entries.push_back(std::move(e));
		}
	}
}

std::vector<wb::SettingDef> startup_source_settings(Ui *ui) {
	std::vector<wb::SettingDef> out;
	if (!ui->script_entries.empty()) {
		wb::SettingDef d;
		d.id = "waybox.autostart.script";
		d.section = _("Started Elsewhere");
		d.title = _("Autostart script");
		d.description = _("Programs your autostart script starts on a line of their own. "
				"Switching one off comments its line out; anything else the script "
				"does is left as it is.");
		if (!ui->script_is_user)
			d.description += std::string(" ") + _("This is the default script: changing "
					"it saves a copy to your configuration.");
		d.keywords = "autostart script startup shell";
		for (const wb::ScriptEntry &e : ui->script_entries)
			d.keywords += " " + e.program;
		d.type = wb::SettingType::Custom;
		d.widget = "autostart-script";
		out.push_back(std::move(d));
	}
	if (!ui->xdg_entries.empty()) {
		wb::SettingDef d;
		d.id = "waybox.autostart.xdg";
		d.section = _("Started Elsewhere");
		d.title = _("Desktop autostart");
		d.description = _("Programs that installed themselves to start with every desktop "
				"session (XDG autostart). Switching one off is saved in your own "
				"autostart folder; the program's file is left alone.");
		d.keywords = "autostart xdg desktop startup";
		for (const wb::AutostartEntry &e : ui->xdg_entries)
			d.keywords += " " + e.name + " " + e.program;
		d.type = wb::SettingType::Custom;
		d.widget = "xdg-autostart";
		out.push_back(std::move(d));
	}
	return out;
}

GtkWidget *build_script_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	for (const wb::ScriptEntry &e : ui->script_entries) {
		auto r = std::make_unique<Ui::SourceRow>();
		r->script = true;
		r->line = e.line;
		r->command = e.command;
		r->program = e.program;
		r->saved = e.enabled;
		char *where = g_strdup_printf(_("line %d"), e.line + 1);
		gtk_box_append(GTK_BOX(box), source_row_widget(ui, r.get(),
				e.program + "  (" + where + ")", e.command));
		g_free(where);
		ui->source_rows.push_back(std::move(r));
	}
	GtkWidget *edit = gtk_button_new_with_label(_("Edit Script…"));
	gtk_widget_set_halign(edit, GTK_ALIGN_START);
	gtk_widget_set_tooltip_text(edit, ui->script_path.c_str());
	g_signal_connect(edit, "clicked", G_CALLBACK(on_edit_script), ui);
	gtk_box_append(GTK_BOX(box), edit);
	return box;
}

GtkWidget *build_xdg_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	for (const wb::AutostartEntry &e : ui->xdg_entries) {
		auto r = std::make_unique<Ui::SourceRow>();
		r->script = false;
		r->file = e.file;
		r->path = e.path;
		r->program = e.program;
		r->saved = e.enabled;
		gtk_box_append(GTK_BOX(box), source_row_widget(ui, r.get(),
				e.name.empty() ? e.file : e.name, e.exec));
		ui->source_rows.push_back(std::move(r));
	}
	return box;
}

void sources_changes(Ui *ui, bool script, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out) {
	for (auto &r : ui->source_rows) {
		if (r->script != script || r->on == nullptr || gtk_switch_get_active(r->on) == r->saved)
			continue;
		std::string name = !r->program.empty() ? r->program
				: script ? r->command : r->file;
		out.push_back({{where + " \xE2\x80\xBA " + name, r->saved ? _("on") : _("off"),
				r->saved ? _("off") : _("on")}, GTK_WIDGET(r->on)});
	}
	(void)target;
}

int sources_unsaved(Ui *ui) {
	int n = 0;
	for (auto &r : ui->source_rows)
		if (r->on != nullptr && gtk_switch_get_active(r->on) != r->saved)
			++n;
	return n;
}

bool save_sources(Ui *ui, std::string *error) {
	std::error_code ec;
	/* The script: re-read, and change only lines still as they were. */
	bool script_changed = false;
	std::string script = ui->script_path.empty() ? "" : read_file(ui->script_path);
	for (auto &r : ui->source_rows) {
		if (!r->script || gtk_switch_get_active(r->on) == r->saved)
			continue;
		bool same = false;
		for (const wb::ScriptEntry &e : wb::script_entries(script))
			if (e.line == r->line && e.command == r->command)
				same = true;
		if (!same) {
			*error = ui->script_path + _(" changed since wbconf read it: reopen wbconf");
			return false;
		}
		script = wb::script_set_enabled(script, r->line, gtk_switch_get_active(r->on));
		script_changed = true;
	}
	if (script_changed) {
		std::string user = config_home() + "/waybox/autostart";
		fs::create_directories(fs::path(user).parent_path(), ec);
		if (ui->script_is_user)
			fs::copy_file(user, user + ".bak", fs::copy_options::overwrite_existing, ec);
		if (!write_file(user, script, error))
			return false;
		/* The session only runs it if it's executable. */
		fs::permissions(user, fs::perms::owner_exec | fs::perms::group_exec |
				fs::perms::others_exec, fs::perm_options::add, ec);
	}
	/* XDG autostart: the user's own copy, switched. */
	std::string dir = config_home() + "/autostart";
	for (auto &r : ui->source_rows) {
		if (r->script || gtk_switch_get_active(r->on) == r->saved)
			continue;
		fs::create_directories(dir, ec);
		std::string text = wb::autostart_set_enabled(read_file(r->path),
				gtk_switch_get_active(r->on));
		if (!write_file(dir + "/" + r->file, text, error))
			return false;
	}
	return true;
}

void sources_saved(Ui *ui) {
	/* What's now in the files: the switches as saved; a changed XDG entry
	 * now has its copy in the user's folder; the script may now be the
	 * user's copy. (Lines don't move: only comment marks changed.) */
	std::string dir = config_home() + "/autostart";
	for (auto &r : ui->source_rows) {
		bool on = gtk_switch_get_active(r->on);
		if (!r->script && on != r->saved)
			r->path = dir + "/" + r->file;
		r->saved = on;
	}
	load_startup_sources(ui);
	for (auto &r : ui->source_rows)
		source_status(ui, r.get());
}

void source_test_value(Ui *ui, const std::string &key, const std::string &value) {
	/* "script.LINE=on|off" (1-based line), "xdg.FILE=on|off". */
	bool on = value == "on";
	for (auto &r : ui->source_rows) {
		if (r->on == nullptr)
			continue;
		if ((key.rfind("script.", 0) == 0 && r->script &&
				std::to_string(r->line + 1) == key.substr(7)) ||
				(key.rfind("xdg.", 0) == 0 && !r->script && r->file == key.substr(4)))
			gtk_switch_set_active(r->on, on);
	}
}

}  // namespace wbconf
