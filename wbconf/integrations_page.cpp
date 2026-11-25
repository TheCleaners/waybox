#include "wbconf_ui.hpp"

namespace wbconf {

/* ---- Integrations ------------------------------------------------------ */

/* The names of running processes (as /proc/PID/comm has them: at most 15
 * characters). */
std::set<std::string> running_processes() {
	std::set<std::string> names;
	DIR *d = opendir("/proc");
	if (d == nullptr)
		return names;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		std::ifstream f(std::string("/proc/") + e->d_name + "/comm");
		std::string comm;
		if (std::getline(f, comm))
			names.insert(comm);
	}
	closedir(d);
	return names;
}

bool is_running(Ui *ui, const std::string &binary) {
	return ui->running.count(binary.substr(0, 15)) > 0;
}

bool installed(const std::string &binary) {
	char *path = g_find_program_in_path(binary.c_str());
	bool found = path != nullptr;
	g_free(path);
	return found;
}

/* The user's autostart script, if any. */
std::string autostart_text() {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	std::string dir = xdg && *xdg ? xdg : home_dir() + "/.config";
	std::ifstream f(dir + "/waybox/autostart");
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void program_status(Ui *ui, Ui::ProgramRow *row) {
	if (row->status == nullptr)
		return;
	std::string binary = row->in ? row->in->binary
		: wb::startup_name("", gtk_editable_get_text(GTK_EDITABLE(row->command)));
	std::string text;
	bool warn = false;
	if (row->in != nullptr && !installed(binary))
		text = _("Not installed");
	else if (is_running(ui, binary))
		text = _("Running");
	if (std::find(ui->in_autostart.begin(), ui->in_autostart.end(), binary) !=
			ui->in_autostart.end() && gtk_switch_get_active(row->on)) {
		text += std::string(text.empty() ? "" : " · ") +
				_("also started by your autostart script: it would run twice");
		warn = true;
	}
	gtk_label_set_text(row->status, text.c_str());
	gtk_widget_set_visible(GTK_WIDGET(row->status), !text.empty());
	if (warn)
		gtk_widget_add_css_class(GTK_WIDGET(row->status), "warning");
	else
		gtk_widget_remove_css_class(GTK_WIDGET(row->status), "warning");
}

void on_program_toggled(GObject *, GParamSpec *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	Ui *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(row->on), "wb-ui"));
	gtk_widget_set_visible(row->details, gtk_switch_get_active(row->on));
	program_status(ui, row);
}

/* Options and the command line stay in step, both ways. */
void on_option_changed(GtkEditable *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	if (row->syncing || row->in == nullptr)
		return;
	std::map<std::string, std::string> values;
	for (auto &[id, e] : row->options)
		values[id] = gtk_editable_get_text(GTK_EDITABLE(e));
	row->syncing = true;
	gtk_editable_set_text(GTK_EDITABLE(row->command), wb::write_options(*row->in,
			gtk_editable_get_text(GTK_EDITABLE(row->command)), values).c_str());
	row->syncing = false;
}

void on_command_changed(GtkEditable *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	if (row->syncing || row->in == nullptr)
		return;
	auto values = wb::read_options(*row->in, gtk_editable_get_text(GTK_EDITABLE(row->command)));
	row->syncing = true;
	for (auto &[id, e] : row->options)
		gtk_editable_set_text(GTK_EDITABLE(e), values.count(id) ? values[id].c_str() : "");
	row->syncing = false;
}

void on_remove_program(GtkButton *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	row->removed = true;
	gtk_widget_set_visible(row->box, FALSE);
}

/* One program's block: switch, name and status; its options; its command. */
Ui::ProgramRow *add_program_row(Ui *ui, GtkWidget *parent, const wb::Integration *in,
		const std::string &name, const std::string &command, bool titled) {
	auto row = std::make_unique<Ui::ProgramRow>();
	row->in = in;
	row->name = name;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	row->box = box;
	gtk_box_append(GTK_BOX(parent), box);

	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(box), head);
	row->on = GTK_SWITCH(gtk_switch_new());
	gtk_widget_set_valign(GTK_WIDGET(row->on), GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(row->on), "wb-ui", ui);
	gtk_box_append(GTK_BOX(head), GTK_WIDGET(row->on));
	if (in != nullptr && !titled) {
		/* In the settings list, whose row shows the name and description:
		 * just "On" beside the switch. */
		GtkWidget *on = gtk_label_new(_("Start with the session"));
		gtk_label_set_xalign(GTK_LABEL(on), 0);
		gtk_box_append(GTK_BOX(head), on);
	} else if (in != nullptr) {
		GtkWidget *title = gtk_label_new(nullptr);
		char *markup = g_markup_printf_escaped("<b>%s</b>  <small>%s</small>",
				in->name.c_str(), in->comment.c_str());
		gtk_label_set_markup(GTK_LABEL(title), markup);
		g_free(markup);
		gtk_label_set_wrap(GTK_LABEL(title), TRUE);
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_widget_set_hexpand(title, TRUE);
		gtk_box_append(GTK_BOX(head), title);
	} else {
		row->name_entry = GTK_ENTRY(gtk_entry_new());
		gtk_entry_set_placeholder_text(row->name_entry, _("Name"));
		gtk_editable_set_text(GTK_EDITABLE(row->name_entry), name.c_str());
		gtk_editable_set_width_chars(GTK_EDITABLE(row->name_entry), 12);
		gtk_box_append(GTK_BOX(head), GTK_WIDGET(row->name_entry));
		GtkWidget *spacer = gtk_label_new("");
		gtk_widget_set_hexpand(spacer, TRUE);
		gtk_box_append(GTK_BOX(head), spacer);
		GtkWidget *remove = gtk_button_new_from_icon_name("list-remove-symbolic");
		gtk_widget_set_tooltip_text(remove, _("Remove"));
		g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_program), row.get());
		gtk_box_append(GTK_BOX(head), remove);
	}

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(body, 52);
	gtk_box_append(GTK_BOX(box), body);
	row->status = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(row->status, 0);
	gtk_label_set_wrap(row->status, TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(row->status), "dim-label");
	gtk_box_append(GTK_BOX(body), GTK_WIDGET(row->status));
	row->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(body), row->details);
	if (in != nullptr && !in->options.empty()) {
		GtkWidget *grid = section_grid(row->details);
		int r = 0;
		for (const wb::IntegrationOption &o : in->options) {
			grid_label(grid, r, (o.label + ":").c_str());
			GtkEntry *e = GTK_ENTRY(grid_control(grid, r++, gtk_entry_new()));
			gtk_entry_set_placeholder_text(e, o.hint.c_str());
			row->options[o.id] = e;
			g_signal_connect(e, "changed", G_CALLBACK(on_option_changed), row.get());
		}
	}
	row->command = GTK_ENTRY(gtk_entry_new());
	gtk_widget_set_hexpand(GTK_WIDGET(row->command), TRUE);
	gtk_entry_set_placeholder_text(row->command, _("Command"));
	gtk_widget_set_tooltip_text(GTK_WIDGET(row->command), _("The command Waybox runs"));
	gtk_box_append(GTK_BOX(row->details), GTK_WIDGET(row->command));
	g_signal_connect(row->command, "changed", G_CALLBACK(on_command_changed), row.get());
	gtk_editable_set_text(GTK_EDITABLE(row->command), command.c_str());
	g_signal_connect(row->on, "notify::active", G_CALLBACK(on_program_toggled), row.get());
	on_program_toggled(nullptr, nullptr, row.get());
	ui->programs.push_back(std::move(row));
	return ui->programs.back().get();
}

void on_add_program(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	Ui::ProgramRow *row = add_program_row(ui, ui->custom_box, nullptr, "", "", true);
	gtk_switch_set_active(row->on, TRUE);
	gtk_widget_grab_focus(GTK_WIDGET(row->command));
}

/* Rows from the loaded <startup>: known programs fill in their row, others
 * get a custom row. */
void populate_programs(Ui *ui, const std::vector<wb::StartupSetting> &startup) {
	for (auto &row : ui->programs) {
		/* Reloading: other programs from the last load go (rc.xml's are
		 * added again below). */
		if (row->in == nullptr && !row->removed) {
			row->removed = true;
			gtk_widget_set_visible(row->box, FALSE);
		}
		row->listed = false;
		row->attrs.clear();
		if (row->in != nullptr) {
			gtk_switch_set_active(row->on, FALSE);
			gtk_editable_set_text(GTK_EDITABLE(row->command), row->in->exec.c_str());
		}
	}
	for (const wb::StartupSetting &p : startup) {
		const wb::Integration *in = wb::match_integration(ui->integrations, p.name, p.command);
		Ui::ProgramRow *row = nullptr;
		for (auto &r : ui->programs)
			if (in != nullptr && r->in == in && !r->listed)
				row = r.get();
		if (row == nullptr)
			row = add_program_row(ui, ui->custom_box, nullptr, p.name, p.command, true);
		row->name = p.name;
		row->attrs = p.attrs;
		row->listed = true;
		gtk_widget_set_visible(row->box, TRUE);
		gtk_editable_set_text(GTK_EDITABLE(row->command), p.command.c_str());
		auto en = p.attrs.find("enabled");
		bool on = en == p.attrs.end() || (en->second != "no" && en->second != "false" &&
				en->second != "0");
		gtk_switch_set_active(row->on, on);
	}
	for (auto &row : ui->programs)
		program_status(ui, row.get());
}

/* <startup> from the rows: those on, and those listed before (kept, off). */
std::vector<wb::StartupSetting> collect_programs(Ui *ui) {
	std::vector<wb::StartupSetting> out;
	std::set<std::string> names;
	for (auto &row : ui->programs) {
		if (row->removed)
			continue;
		bool on = gtk_switch_get_active(row->on);
		if (!on && !row->listed)
			continue;
		wb::StartupSetting p;
		p.command = gtk_editable_get_text(GTK_EDITABLE(row->command));
		if (p.command.empty())
			continue;
		p.attrs = row->attrs;
		if (row->name_entry != nullptr) {
			std::string n = gtk_editable_get_text(GTK_EDITABLE(row->name_entry));
			if (!n.empty())
				p.attrs["name"] = n;
			else
				p.attrs.erase("name");
		} else if (!row->listed) {
			p.attrs["name"] = row->in->id;  /* so wbconf finds it again */
		}
		p.name = wb::startup_name(p.attrs.count("name") ? p.attrs["name"] : "", p.command);
		if (on) {
			auto en = p.attrs.find("enabled");
			if (en != p.attrs.end() && en->second != "yes")
				p.attrs.erase(en);
		} else {
			p.attrs["enabled"] = "no";
		}
		if (!names.insert(p.name).second)
			continue;  /* two with one name: the first */
		out.push_back(std::move(p));
	}
	return out;
}

/* The known programs by category, then other programs. */
const char *integration_category_title(const std::string &category) {
	static const std::map<std::string, const char *> titles = {
		{"Panel", N_("Panel")}, {"Notifications", N_("Notifications")},
		{"Wallpaper", N_("Wallpaper")}, {"NightLight", N_("Night Light")},
		{"Idle", N_("Idle and Locking")}, {"Clipboard", N_("Clipboard")},
		{"Pickers", N_("Pickers")},
		{"Tray", N_("Tray Applets")}, {"Polkit", N_("Password Prompts")},
		{"Displays", N_("Displays")}, {"Screenshots", N_("Screenshots")},
	};
	auto t = titles.find(category);
	return t != titles.end() ? _(t->second) : category.c_str();
}

/* The integration files, what's running, what the autostart script runs,
 * and which programs rc.xml lists: what the settings list is built from. */
void load_integrations_data(Ui *ui, const wb::WayboxSettings &s) {
	const char *home = getenv("HOME");
	const char *xdh = getenv("XDG_CONFIG_HOME");
	const char *xdd = getenv("XDG_DATA_DIRS");
	ui->integrations = wb::load_integrations(wb::integration_dirs(home ? home : "",
			xdh ? xdh : "", xdd ? xdd : "", WB_INTEGRATIONS_DIR));
	ui->running = running_processes();
	std::vector<std::string> binaries;
	for (const wb::Integration &in : ui->integrations)
		binaries.push_back(in.binary);
	ui->in_autostart = wb::programs_in_script(autostart_text(), binaries);
	ui->listed_integrations.clear();
	for (const wb::StartupSetting &p : s.startup)
		if (const wb::Integration *in = wb::match_integration(ui->integrations, p.name, p.command))
			ui->listed_integrations.insert(in->id);
}

/* The settings list's entries for programs: one per known program that's
 * installed or in rc.xml, by category, then "Other programs". */
std::vector<wb::SettingDef> program_settings(Ui *ui) {
	std::vector<wb::SettingDef> out;
	std::vector<std::string> missing;
	for (const wb::Integration &in : ui->integrations) {
		if (!installed(in.binary) && ui->listed_integrations.count(in.id) == 0) {
			missing.push_back(in.name);
			continue;
		}
		wb::SettingDef d;
		d.id = "waybox.startup." + in.id;
		d.section = integration_category_title(in.category);
		d.title = in.name;
		d.description = in.comment;
		d.keywords = in.binary + " " + in.category + " autostart startup";
		for (const wb::IntegrationOption &o : in.options)
			d.keywords += " " + o.label;
		d.type = wb::SettingType::Custom;
		d.widget = (in.binding ? "binding:" : "program:") + in.id;
		if (in.binding)
			d.keywords += " key binding shortcut";
		out.push_back(std::move(d));
	}
	wb::SettingDef other;
	other.id = "waybox.startup.other";
	other.section = _("Other Programs");
	other.title = _("Other programs");
	other.description = _("Any command, started with the session and again if it crashes.");
	if (!missing.empty()) {
		std::string list;
		for (const std::string &m : missing)
			list += (list.empty() ? "" : ", ") + m;
		other.description += std::string(" ") + _("Also supported when installed: ") + list + ".";
	}
	other.keywords = "autostart startup command custom";
	other.type = wb::SettingType::Custom;
	other.widget = "programs-other";
	out.push_back(std::move(other));
	return out;
}

/* One known program's editor: its switch, status, options and command. */
GtkWidget *build_program_block(Ui *ui, const std::string &id) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (const wb::Integration &in : ui->integrations)
		if (in.id == id)
			add_program_row(ui, box, &in, in.id, in.exec, false);
	return box;
}

/* Other programs: their rows, and a button to add one. */
GtkWidget *build_other_programs_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	ui->custom_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(box), ui->custom_box);
	GtkWidget *add = gtk_button_new_with_label(_("Add a Program"));
	gtk_widget_set_halign(add, GTK_ALIGN_START);
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_program), ui);
	gtk_box_append(GTK_BOX(box), add);
	return box;
}

/* ---- Programs run from a key ----------------------------------------- */

namespace {

std::optional<std::pair<std::string, std::string>> binding_now(Ui::BindingRow *b) {
	if (!gtk_switch_get_active(b->on))
		return std::nullopt;
	std::string key = gtk_editable_get_text(GTK_EDITABLE(b->key));
	std::string cmd = gtk_editable_get_text(GTK_EDITABLE(b->command));
	if (key.empty() || cmd.empty())
		return std::nullopt;
	return std::make_pair(key, cmd);
}

void binding_status(Ui *ui, Ui::BindingRow *b) {
	std::string text;
	bool warn = false;
	if (!installed(b->in->binary)) {
		text = _("Not installed");
	} else if (gtk_switch_get_active(b->on)) {
		std::string key = gtk_editable_get_text(GTK_EDITABLE(b->key));
		bool ours = b->saved && b->saved->first == key;
		if (!ours && std::find(ui->bound_keys.begin(), ui->bound_keys.end(), key) !=
				ui->bound_keys.end()) {
			text = key + " " + _("already does something else: saving replaces it");
			warn = true;
		}
	}
	gtk_label_set_text(b->status, text.c_str());
	gtk_widget_set_visible(GTK_WIDGET(b->status), !text.empty());
	if (warn)
		gtk_widget_add_css_class(GTK_WIDGET(b->status), "warning");
	else
		gtk_widget_remove_css_class(GTK_WIDGET(b->status), "warning");
}

void on_binding_changed(Ui::BindingRow *b) {
	Ui *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(b->on), "wb-ui"));
	gtk_widget_set_visible(b->details, gtk_switch_get_active(b->on));
	binding_status(ui, b);
}

}  // namespace

GtkWidget *build_binding_block(Ui *ui, const std::string &id) {
	const wb::Integration *in = nullptr;
	for (const wb::Integration &i : ui->integrations)
		if (i.id == id)
			in = &i;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	if (in == nullptr)
		return box;
	auto b = std::make_unique<Ui::BindingRow>();
	b->in = in;
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	b->on = GTK_SWITCH(gtk_switch_new());
	gtk_widget_set_valign(GTK_WIDGET(b->on), GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(b->on), "wb-ui", ui);
	gtk_box_append(GTK_BOX(head), GTK_WIDGET(b->on));
	gtk_box_append(GTK_BOX(head), gtk_label_new(_("Bind to a key")));
	gtk_box_append(GTK_BOX(box), head);
	b->status = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(b->status, 0);
	gtk_label_set_wrap(b->status, TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(b->status), "dim-label");
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->status));

	b->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *grid = section_grid(b->details);
	grid_label(grid, 0, _("Key:"));
	b->key = GTK_ENTRY(grid_control(grid, 0, gtk_entry_new()));
	gtk_entry_set_placeholder_text(b->key, in->key.c_str());
	GtkWidget *hint = gtk_label_new(_("W Super, A Alt, C Ctrl, S Shift: W-period is Super+."));
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), hint, 2, 0, 1, 1);
	b->command = GTK_ENTRY(gtk_entry_new());
	gtk_widget_set_hexpand(GTK_WIDGET(b->command), TRUE);
	gtk_entry_set_placeholder_text(b->command, _("Command"));
	gtk_box_append(GTK_BOX(b->details), GTK_WIDGET(b->command));
	gtk_box_append(GTK_BOX(box), b->details);

	g_signal_connect_swapped(b->on, "notify::active", G_CALLBACK(on_binding_changed), b.get());
	g_signal_connect_swapped(b->key, "changed", G_CALLBACK(on_binding_changed), b.get());
	ui->bindings.push_back(std::move(b));
	return box;
}

void populate_bindings(Ui *ui, const wb::RcDocument *doc) {
	std::vector<std::pair<std::string, std::string>> binds;
	ui->bound_keys.clear();
	if (doc != nullptr) {
		binds = doc->execute_keybinds();
		ui->bound_keys = doc->keybind_keys();
	}
	for (auto &b : ui->bindings) {
		b->saved.reset();
		/* Bound already if a key runs this program. */
		for (const auto &[key, cmd] : binds)
			if (wb::startup_name("", cmd) == b->in->binary) {
				b->saved = std::make_pair(key, cmd);
				break;
			}
		gtk_editable_set_text(GTK_EDITABLE(b->key),
				(b->saved ? b->saved->first : b->in->key).c_str());
		gtk_editable_set_text(GTK_EDITABLE(b->command),
				(b->saved ? b->saved->second : b->in->exec).c_str());
		gtk_switch_set_active(b->on, b->saved.has_value());
		on_binding_changed(b.get());
	}
}

void apply_bindings(Ui *ui, wb::RcDocument &doc) {
	for (auto &b : ui->bindings) {
		auto now = binding_now(b.get());
		if (now == b->saved)
			continue;
		if (b->saved)
			doc.set_keybind(b->saved->first, std::nullopt);
		if (now)
			doc.set_keybind(now->first, now->second);
	}
}

void bindings_saved(Ui *ui) {
	for (auto &b : ui->bindings) {
		b->saved = binding_now(b.get());
		if (b->saved && std::find(ui->bound_keys.begin(), ui->bound_keys.end(),
				b->saved->first) == ui->bound_keys.end())
			ui->bound_keys.push_back(b->saved->first);
		binding_status(ui, b.get());
	}
}

int bindings_unsaved(Ui *ui) {
	int n = 0;
	for (auto &b : ui->bindings)
		n += binding_now(b.get()) != b->saved;
	return n;
}

bool binding_set(Ui *ui, const std::string &id) {
	for (auto &b : ui->bindings)
		if (b->in->id == id)
			return b->saved.has_value() || gtk_switch_get_active(b->on);
	return false;
}

}  // namespace wbconf
