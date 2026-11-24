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
		const std::string &name, const std::string &command) {
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
	if (in != nullptr) {
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
	Ui::ProgramRow *row = add_program_row(ui, ui->custom_box, nullptr, "", "");
	gtk_switch_set_active(row->on, TRUE);
	gtk_widget_grab_focus(GTK_WIDGET(row->command));
}

/* Rows from the loaded <startup>: known programs fill in their row, others
 * get a custom row. */
void populate_programs(Ui *ui, const std::vector<wb::StartupSetting> &startup) {
	for (auto &row : ui->programs) {
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
			row = add_program_row(ui, ui->custom_box, nullptr, p.name, p.command);
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

GtkWidget *build_integrations_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), inner);
	gtk_box_append(GTK_BOX(page), scroller);

	GtkWidget *note = gtk_label_new(_("Programs Waybox starts with the session, "
			"and starts again if they crash. Nothing is started unless you "
			"turn it on here (or list it in rc.xml)."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(inner), note);

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

	/* Known programs by category; ones not installed are listed by name
	 * only (shown in full if rc.xml has them). */
	static const std::map<std::string, const char *> titles = {
		{"Panel", N_("Panel")}, {"Notifications", N_("Notifications")},
		{"Wallpaper", N_("Wallpaper")}, {"NightLight", N_("Night Light")},
		{"Idle", N_("Idle and Locking")}, {"Clipboard", N_("Clipboard")},
		{"Tray", N_("Tray Applets")}, {"Polkit", N_("Password Prompts")},
		{"Displays", N_("Displays")}, {"Screenshots", N_("Screenshots")},
	};
	std::string category;
	GtkWidget *content = nullptr;
	std::vector<std::string> missing;
	for (const wb::Integration &in : ui->integrations) {
		if (in.category != category || content == nullptr) {
			category = in.category;
			auto t = titles.find(category);
			content = section(inner, t != titles.end() ? _(t->second) : category.c_str());
		}
		Ui::ProgramRow *row = add_program_row(ui, content, &in, in.id, in.exec);
		if (!installed(in.binary)) {
			gtk_widget_set_visible(row->box, FALSE);  /* shown if listed */
			missing.push_back(in.name);
		}
	}
	GtkWidget *grid = section_grid(section(inner, _("Screens")));
	ui->screen_off = add_spin(grid, 0, _("Switch the screens off after (minutes, 0 never):"),
			0, 240);

	ui->custom_box = section(inner, _("Other Programs"));
	GtkWidget *add = gtk_button_new_with_label(_("Add a Program"));
	gtk_widget_set_halign(add, GTK_ALIGN_START);
	g_signal_connect(add, "clicked", G_CALLBACK(on_add_program), ui);
	gtk_box_append(GTK_BOX(inner), add);

	if (!missing.empty()) {
		std::string list;
		for (const std::string &m : missing)
			list += (list.empty() ? "" : ", ") + m;
		GtkWidget *more = gtk_label_new((std::string(_("Also supported when installed: ")) +
				list + ".").c_str());
		gtk_label_set_wrap(GTK_LABEL(more), TRUE);
		gtk_label_set_xalign(GTK_LABEL(more), 0);
		gtk_widget_add_css_class(more, "dim-label");
		gtk_box_append(GTK_BOX(inner), more);
	}
	return page;
}

}  // namespace wbconf
