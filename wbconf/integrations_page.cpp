#include "wbconf_ui.hpp"

#include <sstream>

#include <fstream>

#include <unistd.h>

#include <filesystem>

#include <csignal>

namespace wbconf {

namespace fs = std::filesystem;

/* ---- Integrations ------------------------------------------------------ */

/* The names of running processes (as /proc/PID/comm has them: at most 15
 * characters). */
std::set<std::string> running_processes() {
	std::set<std::string> names;
	for (const ProcessInfo &p : list_processes())
		names.insert(p.name);
	return names;
}

bool is_running(Ui *ui, const std::string &binary) {
	return ui->running.count(binary.substr(0, 15)) > 0;
}

bool installed(const std::string &binary) {
	/* A full path (helpers in /usr/libexec aren't on PATH). */
	if (binary.find('/') != std::string::npos)
		return access(binary.c_str(), X_OK) == 0;
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
	bool scripted = std::find(ui->in_autostart.begin(), ui->in_autostart.end(),
			binary) != ui->in_autostart.end();
	wb::ProgramStatus st = wb::program_status(row->in == nullptr || installed(binary),
			is_running(ui, binary), scripted, gtk_switch_get_active(row->on));
	std::string text;
	auto add = [&text](const char *part) {
		text += std::string(text.empty() ? "" : " · ") + part;
	};
	if (st.not_installed)
		add(_("Not installed"));
	else if (st.running)
		add(_("Running"));
	if (!st.not_installed) {
		auto v = ui->versions.find(binary);
		if (v != ui->versions.end() && !v->second.empty()) {
			char *text_v = g_strdup_printf(_("version %s"), v->second.c_str());
			add(text_v);
			g_free(text_v);
		}
	}
	if (st.twice)
		add(_("also started by your autostart script: it would run twice"));
	else if (st.from_script)
		add(_("started by your autostart script"));
	bool warn = st.twice;
	gtk_label_set_text(row->status, text.c_str());
	gtk_widget_set_visible(GTK_WIDGET(row->status), !text.empty());
	if (warn)
		gtk_widget_add_css_class(GTK_WIDGET(row->status), "warning");
	else
		gtk_widget_remove_css_class(GTK_WIDGET(row->status), "warning");
}

/* ---- versions ------------------------------------------------------------ */

struct VersionJob {
	Ui *ui;
	std::string binary;
	GSubprocess *proc;
	guint timeout = 0;
};

void version_done(GObject *source, GAsyncResult *result, gpointer data) {
	auto *job = static_cast<VersionJob *>(data);
	char *out = nullptr;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, nullptr, nullptr);
	if (job->timeout != 0)
		g_source_remove(job->timeout);
	Ui *ui = job->ui;
	ui->versions[job->binary] = out != nullptr ? wb::version_from_output(out) : "";
	g_log("wbconf", G_LOG_LEVEL_DEBUG, "version of %s: \"%s\"", job->binary.c_str(),
			ui->versions[job->binary].c_str());
	g_free(out);
	if (ui->window != nullptr)
		for (auto &row : ui->programs)
			if (row->in != nullptr && row->in->binary == job->binary && row->status != nullptr)
				program_status(ui, row.get());
	g_object_unref(job->proc);
	delete job;
}

/* Ask each installed program with a Version= command for its version, in
 * the background. It runs without the session's display or bus, so a
 * program that doesn't know the flag and starts can't show up or take
 * over a service; and it gets three seconds. */
void fetch_versions(Ui *ui) {
	for (const wb::Integration &in : ui->integrations) {
		if (in.version_command.empty() || ui->versions.count(in.binary) || !installed(in.binary))
			continue;
		ui->versions[in.binary] = "";
		gchar **argv = nullptr;
		if (!g_shell_parse_argv(in.version_command.c_str(), nullptr, &argv, nullptr))
			continue;
		GSubprocessLauncher *l = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(
				G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
				G_SUBPROCESS_FLAGS_STDERR_MERGE));
		for (const char *var : {"WAYLAND_DISPLAY", "DISPLAY", "WAYLAND_SOCKET"})
			g_subprocess_launcher_unsetenv(l, var);
		g_subprocess_launcher_setenv(l, "DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
		GSubprocess *proc = g_subprocess_launcher_spawnv(l, argv, nullptr);
		g_object_unref(l);
		g_strfreev(argv);
		if (proc == nullptr)
			continue;
		auto *job = new VersionJob{ui, in.binary, proc};
		job->timeout = g_timeout_add_seconds(3, [](gpointer data) -> gboolean {
			auto *job = static_cast<VersionJob *>(data);
			job->timeout = 0;
			g_subprocess_force_exit(job->proc);
			return G_SOURCE_REMOVE;
		}, job);
		g_subprocess_communicate_utf8_async(proc, nullptr, nullptr, version_done, job);
	}
}

/* ---- moving a program from the autostart script to Waybox -------------- */

std::string program_binary(Ui::ProgramRow *row) {
	return row->in ? row->in->binary
		: wb::startup_name("", gtk_editable_get_text(GTK_EDITABLE(row->command)));
}

/* The user's running copies of `binary`. */
std::vector<pid_t> running_copies(const std::string &binary) {
	std::vector<pid_t> out;
	uid_t me = getuid();
	std::string name = binary.substr(0, 15);
	for (const ProcessInfo &p : list_processes())
		if (p.uid == me && p.name == name)
			out.push_back(p.pid);
	return out;
}

/* Let Waybox start it: the script's lines that start it switched off
 * (saved with the rest); `stop`: its running copies go when saving. */
/* The command Waybox should run in its place: a line of its own's command,
 * with its arguments; otherwise (a function) the row's. */
std::string proposed_command(Ui::ProgramRow *row, const wb::ScriptStarters &starters) {
	if (starters.lines.size() == 1 && !starters.lines[0].function)
		return wb::script_start_command(starters.lines[0].command);
	return gtk_editable_get_text(GTK_EDITABLE(row->command));
}

void move_to_waybox(Ui *ui, Ui::ProgramRow *row, const wb::ScriptStarters &starters, bool stop,
		const std::vector<pid_t> &running, const std::string &command) {
	if (!command.empty())
		gtk_editable_set_text(GTK_EDITABLE(row->command), command.c_str());
	for (const wb::ScriptEntry &e : starters.lines)
		for (auto &r : ui->source_rows)
			if (r->script && r->line == e.line && r->on != nullptr)
				gtk_switch_set_active(r->on, FALSE);
	row->stop_on_save = stop && !running.empty();
	row->stop_pids = row->stop_on_save ? running : std::vector<pid_t>{};
	row->via_function = !starters.functions.empty();
	program_status(ui, row);
	refresh_save_state(ui);
}

void leave_to_script(Ui *ui, Ui::ProgramRow *row) {
	row->stop_on_save = false;
	row->stop_pids.clear();
	gtk_switch_set_active(row->on, FALSE);  /* asked already: not again */
	program_status(ui, row);
}

struct MoveDialog {
	Ui *ui = nullptr;
	Ui::ProgramRow *row = nullptr;
	wb::ScriptStarters starters;
	GtkWindow *window = nullptr;
	GtkCheckButton *stop = nullptr;
	GtkEntry *command = nullptr;
	std::vector<pid_t> running;
	bool decided = false;
};

void move_dialog_answer(MoveDialog *d, bool to_waybox) {
	if (d->decided)
		return;
	d->decided = true;
	if (to_waybox)
		move_to_waybox(d->ui, d->row, d->starters,
				d->stop != nullptr && gtk_check_button_get_active(d->stop), d->running,
				gtk_editable_get_text(GTK_EDITABLE(d->command)));
	else
		leave_to_script(d->ui, d->row);
	gtk_window_destroy(d->window);
}

/* Turned on, but the autostart script starts it too: which should? */
void ask_move(Ui *ui, Ui::ProgramRow *row, const wb::ScriptStarters &starters) {
	std::string binary = program_binary(row);
	std::vector<pid_t> running = running_copies(binary);
	if (!ui->test_choice.empty()) {
		if (ui->test_choice.rfind("waybox", 0) == 0)
			move_to_waybox(ui, row, starters, ui->test_choice == "waybox+stop", running,
					proposed_command(row, starters));
		else
			leave_to_script(ui, row);
		return;
	}
	auto *d = new MoveDialog;
	d->ui = ui;
	d->row = row;
	d->starters = starters;
	d->running = running;
	GtkWidget *win = gtk_window_new();
	d->window = GTK_WINDOW(win);
	gtk_window_set_transient_for(d->window, ui->window);
	gtk_window_set_modal(d->window, TRUE);
	gtk_window_set_resizable(d->window, FALSE);
	gtk_window_set_title(d->window, _("Started Twice"));
	g_object_set_data_full(G_OBJECT(win), "wb-move", d,
			[](gpointer p) { delete static_cast<MoveDialog *>(p); });

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(box, 18);
	gtk_widget_set_margin_bottom(box, 18);
	gtk_widget_set_margin_start(box, 18);
	gtk_widget_set_margin_end(box, 18);
	gtk_window_set_child(d->window, box);
	auto label = [box](const std::string &text, const char *css = nullptr) {
		GtkWidget *l = gtk_label_new(text.c_str());
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		if (css != nullptr)
			gtk_widget_add_css_class(l, css);
		gtk_box_append(GTK_BOX(box), l);
		return l;
	};
	char *head = g_strdup_printf(_("%s is also started by your autostart script"),
			binary.c_str());
	label(head, "title-4");
	g_free(head);
	label(_("Letting Waybox start it switches off the script's lines below when you "
			"save; Waybox then starts it with the session and restarts it if it "
			"crashes. The lines stay in the script, commented out."));
	for (const wb::ScriptEntry &e : starters.lines) {
		char *where = g_strdup_printf(_("Line %d:"), e.line + 1);
		label(std::string(where) + " " + e.command, "monospace");
		g_free(where);
		if (e.function) {
			char *note = g_strdup_printf(_("This runs %s, a function of the script that "
					"starts it: switching the line off stops everything %s does."),
					e.program.c_str(), e.program.c_str());
			label(note, "dim-label");
			g_free(note);
		}
	}
	if (starters.elsewhere)
		label(_("The script also starts it in a way wbconf can't switch off (inside an "
				"if block, for example): edit the script to stop that too."), "warning");

	/* What Waybox will run instead, which the user can change here. */
	label(_("Waybox will run:"));
	d->command = GTK_ENTRY(gtk_entry_new());
	gtk_editable_set_text(GTK_EDITABLE(d->command), proposed_command(row, starters).c_str());
	gtk_widget_add_css_class(GTK_WIDGET(d->command), "monospace");
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(d->command));
	if (!starters.functions.empty()) {
		char *note = g_strdup_printf(_("%s may do more than this command does (pick a "
				"random image, or change it now and then, for example): check the "
				"command before letting Waybox start it."), starters.functions[0].c_str());
		label(note, "warning");
		g_free(note);
		/* Others of its kind might do what the function did. */
		if (row->in != nullptr) {
			std::string others;
			for (const wb::Integration &in : ui->integrations)
				if (in.category == row->in->category && in.id != row->in->id)
					others += (others.empty() ? "" : ", ") + in.name;
			if (!others.empty()) {
				char *alt = g_strdup_printf(_("Other programs of this kind, in Integrations: "
						"%s."), others.c_str());
				label(alt, "dim-label");
				g_free(alt);
			}
		}
	}
	if (!running.empty()) {
		std::string pids;
		for (pid_t p : running)
			pids += (pids.empty() ? "" : ", ") + std::to_string(p);
		char *text = starters.functions.empty()
			? g_strdup_printf(ngettext(
				"Also stop the running %s (process %s) when saving",
				"Also stop the running %s (processes %s) when saving",
				running.size()), binary.c_str(), pids.c_str())
			: g_strdup_printf(ngettext(
				"Also stop the running %s (process %s), and the script's %s if it's "
				"still running, when saving",
				"Also stop the running %s (processes %s), and the script's %s if it's "
				"still running, when saving",
				running.size()), binary.c_str(), pids.c_str(),
				starters.functions[0].c_str());
		d->stop = GTK_CHECK_BUTTON(gtk_check_button_new());
		GtkWidget *stop_label = gtk_label_new(text);
		gtk_label_set_wrap(GTK_LABEL(stop_label), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(stop_label), 56);
		gtk_label_set_xalign(GTK_LABEL(stop_label), 0);
		gtk_check_button_set_child(d->stop, stop_label);
		g_free(text);
		gtk_check_button_set_active(d->stop, TRUE);
		gtk_widget_set_tooltip_text(GTK_WIDGET(d->stop), _("Otherwise the copy the script "
				"started keeps running until you log out, next to Waybox's."));
		gtk_box_append(GTK_BOX(box), GTK_WIDGET(d->stop));
	}
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(box), buttons);
	GtkWidget *keep = gtk_button_new_with_label(_("Leave It to the Script"));
	GtkWidget *move = gtk_button_new_with_label(_("Let Waybox Start It"));
	gtk_widget_add_css_class(move, "suggested-action");
	gtk_box_append(GTK_BOX(buttons), keep);
	gtk_box_append(GTK_BOX(buttons), move);
	g_signal_connect_swapped(keep, "clicked", G_CALLBACK(+[](MoveDialog *d) {
		move_dialog_answer(d, false);
	}), d);
	g_signal_connect_swapped(move, "clicked", G_CALLBACK(+[](MoveDialog *d) {
		move_dialog_answer(d, true);
	}), d);
	/* Closed some other way: as if left to the script. */
	g_signal_connect_swapped(win, "close-request", G_CALLBACK(+[](MoveDialog *d) -> gboolean {
		if (!d->decided) {
			d->decided = true;
			leave_to_script(d->ui, d->row);
		}
		return FALSE;
	}), d);
	gtk_window_set_default_widget(d->window, move);
	gtk_window_present(d->window);
}

void stop_moved_programs(Ui *ui) {
	std::string script_name = fs::path(ui->script_path).filename().string().substr(0, 15);
	uid_t me = getuid();
	for (auto &row : ui->programs) {
		if (!row->stop_on_save)
			continue;
		row->stop_on_save = false;
		if (row->removed || !gtk_switch_get_active(row->on))
			continue;
		std::string name = program_binary(row.get()).substr(0, 15);
		std::vector<pid_t> pids = std::move(row->stop_pids);
		row->stop_pids.clear();
		std::vector<ProcessInfo> all = list_processes();
		for (const ProcessInfo &p : all) {
			/* Only the copies shown when asked, if still the same program
			 * (a process id can be reused). */
			if (p.uid != me || p.name != name ||
					std::find(pids.begin(), pids.end(), p.pid) == pids.end())
				continue;
			/* Started from a function of the script: the background shell
			 * running it goes first, or it could start another. It's the
			 * script's: named after it (Linux), or running its file (the
			 * BSDs name it after the shell). A function's line ends in
			 * "&", so that shell is never the script itself; and a
			 * terminal's shell never matches. */
			if (row->via_function && !script_name.empty())
				for (const ProcessInfo &parent : all) {
					if (parent.pid != p.ppid || parent.uid != me || parent.pid <= 1)
						continue;
					std::vector<std::string> args = process_args(parent.pid);
					bool runs_script = std::find(args.begin(), args.end(), ui->script_path) !=
							args.end();
					if (parent.name == script_name || runs_script)
						kill(parent.pid, SIGTERM);
				}
			kill(p.pid, SIGTERM);
		}
	}
}

void on_program_toggled(GObject *, GParamSpec *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	Ui *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(row->on), "wb-ui"));
	gtk_widget_set_visible(row->details, gtk_switch_get_active(row->on));
	program_status(ui, row);
	if (!gtk_switch_get_active(row->on)) {
		row->stop_on_save = false;
		return;
	}
	if (!ui->ready || row->asked || row->listed || ui->script_path.empty())
		return;
	/* The script the session runs (the user's, else the default): the
	 * one whose lines the Startup view switches. */
	std::ifstream f(ui->script_path);
	std::stringstream text;
	text << f.rdbuf();
	wb::ScriptStarters starters = wb::script_starters(text.str(), program_binary(row));
	if (starters.lines.empty() && !starters.elsewhere)
		return;
	row->asked = true;
	ask_move(ui, row, starters);
}

/* Configure=: the program's own settings tool, started apart from wbconf
 * (it stays open when wbconf closes). No shell: the command's words. */
bool configure_installed(const wb::Integration *in) {
	if (in == nullptr || in->configure.empty())
		return false;
	std::vector<std::string> words = wb::split_command(in->configure);
	return !words.empty() && installed(words[0]);
}

void open_configure(const wb::Integration *in) {
	std::vector<std::string> words = wb::split_command(in->configure);
	if (words.empty())
		return;
	std::vector<char *> argv;
	for (std::string &w : words)
		argv.push_back(w.data());
	argv.push_back(nullptr);
	GPid pid = 0;
	GError *error = nullptr;
	if (!g_spawn_async(nullptr, argv.data(), nullptr,
			static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
			[](gpointer) { setsid(); }, nullptr, &pid, &error)) {
		g_warning("can't start %s: %s", in->configure.c_str(), error->message);
		g_clear_error(&error);
		return;
	}
	g_child_watch_add(pid, [](GPid pid, gint, gpointer) { g_spawn_close_pid(pid); }, nullptr);
}

GtkWidget *configure_button(const wb::Integration *in) {
	GtkWidget *b = gtk_button_new_with_label(_("Configure…"));
	char *tip = g_strdup_printf(_("Open %s"), in->configure.c_str());
	gtk_widget_set_tooltip_text(b, tip);
	g_free(tip);
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	g_object_set_data(G_OBJECT(b), "wb-integration", const_cast<wb::Integration *>(in));
	g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
		auto *in = static_cast<const wb::Integration *>(
				g_object_get_data(G_OBJECT(b), "wb-integration"));
		if (in != nullptr)
			open_configure(in);
	}), nullptr);
	return b;
}

GtkWidget *build_integration_page(Ui *ui, Ui::ProgramRow *row) {
	if (row->in == nullptr)
		return nullptr;
	if (row->in->page == "wallpaper")
		return build_wallpaper_page(ui, row);
	return nullptr;
}

/* Options and the command line stay in step, both ways. */
void on_option_changed(Ui::ProgramRow *row) {
	if (row->syncing || row->in == nullptr)
		return;
	std::map<std::string, std::string> values;
	for (auto &[id, f] : row->options)
		values[id] = option_field_value(*f);
	row->syncing = true;
	gtk_editable_set_text(GTK_EDITABLE(row->command), wb::write_options(*row->in,
			gtk_editable_get_text(GTK_EDITABLE(row->command)), values).c_str());
	row->syncing = false;
	if (row->page_refresh)
		row->page_refresh();
}

void on_command_changed(GtkEditable *, gpointer data) {
	auto *row = static_cast<Ui::ProgramRow *>(data);
	if (row->syncing || row->in == nullptr)
		return;
	auto values = wb::read_options(*row->in, gtk_editable_get_text(GTK_EDITABLE(row->command)));
	row->syncing = true;
	for (auto &[id, f] : row->options)
		set_option_field_value(*f, values.count(id) ? values[id] : "");
	row->syncing = false;
	if (row->page_refresh)
		row->page_refresh();
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
	/* Its own settings tool: there whether it's on or not (azote, say, is
	 * where its wallpaper is chosen before it's started). */
	if (configure_installed(in)) {
		row->configure = configure_button(in);
		gtk_box_append(GTK_BOX(body), row->configure);
	}
	row->status = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(row->status, 0);
	gtk_label_set_wrap(row->status, TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(row->status), "dim-label");
	gtk_box_append(GTK_BOX(body), GTK_WIDGET(row->status));
	row->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(body), row->details);
	if (in != nullptr && !in->options.empty()) {
		/* With a page, the options go under it, folded. */
		GtkWidget *options_parent = row->details;
		GtkWidget *expander = nullptr;
		if (!in->page.empty()) {
			expander = gtk_expander_new(_("All options"));
			options_parent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
			gtk_expander_set_child(GTK_EXPANDER(expander), options_parent);
		}
		GtkWidget *grid = section_grid(options_parent);
		int r = 0;
		for (const wb::IntegrationOption &o : in->options) {
			grid_label(grid, r, (o.label + ":").c_str());
			Ui::ProgramRow *rp = row.get();
			auto f = make_option_field(o, [rp] { on_option_changed(rp); });
			grid_control(grid, r++, f->widget);
			if (o.type != wb::OptionType::Switch)
				gtk_widget_set_hexpand(f->widget, TRUE);
			row->options[o.id] = std::move(f);
		}
		if (expander != nullptr) {
			if (GtkWidget *page = build_integration_page(ui, row.get()))
				gtk_box_append(GTK_BOX(row->details), page);
			gtk_box_append(GTK_BOX(row->details), expander);
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
	load_startup_sources(ui);
	fetch_versions(ui);
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
		if (in.input_method)
			continue;  /* chosen under Keyboard: Input method */
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
	for (wb::SettingDef &d : startup_source_settings(ui))
		out.push_back(std::move(d));
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

void binding_changes(Ui *ui, const std::string &id, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out) {
	auto text = [](const std::optional<std::pair<std::string, std::string>> &b) {
		return b ? b->first + "  " + b->second : std::string(_("off"));
	};
	for (auto &b : ui->bindings)
		if (b->in != nullptr && b->in->id == id) {
			auto now = binding_now(b.get());
			if (now != b->saved)
				out.push_back({{where, text(b->saved), text(now)}, target});
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

/* ---- The input method ------------------------------------------------- */

namespace {

std::string env_text(const std::vector<std::pair<std::string, std::string>> &env) {
	std::string out;
	for (const auto &[name, value] : env)
		out += name + "=" + value + "\n";
	return out;
}

void set_env_text(Ui *ui, const std::string &text) {
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(ui->im.env), text.c_str(), -1);
}

/* The chosen framework's settings tool, if it has one installed. */
void update_im_configure(Ui *ui) {
	if (ui->im.configure == nullptr)
		return;
	guint idx = gtk_drop_down_get_selected(ui->im.choice);
	const wb::Integration *in = idx >= 1 && idx <= ui->im.frameworks.size()
			? ui->im.frameworks[idx - 1] : nullptr;
	bool show = configure_installed(in);
	if (show) {
		g_object_set_data(G_OBJECT(ui->im.configure), "wb-integration",
				const_cast<wb::Integration *>(in));
		char *tip = g_strdup_printf(_("Open %s"), in->configure.c_str());
		gtk_widget_set_tooltip_text(ui->im.configure, tip);
		g_free(tip);
	}
	gtk_widget_set_visible(ui->im.configure, show);
}

/* The dropdown changed: show (and for a framework, fill) the details. */
void on_im_choice(Ui *ui) {
	guint idx = gtk_drop_down_get_selected(ui->im.choice);
	size_t n = ui->im.frameworks.size();
	gtk_widget_set_visible(ui->im.details, idx != 0);
	update_im_configure(ui);
	if (ui->im.updating || idx == 0 || idx > n)
		return;
	const wb::Integration *in = ui->im.frameworks[idx - 1];
	gtk_editable_set_text(GTK_EDITABLE(ui->im.command), in->exec.c_str());
	set_env_text(ui, env_text(in->env));
	update_im_configure(ui);
}

}  // namespace

GtkWidget *build_input_method_block(Ui *ui) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	std::vector<std::string> labels = {_("None")};
	for (const wb::Integration &in : ui->integrations)
		if (in.input_method && installed(in.binary)) {
			ui->im.frameworks.push_back(&in);
			labels.push_back(in.name);
		}
	labels.push_back(_("Custom"));
	std::vector<const char *> items;
	for (const std::string &l : labels)
		items.push_back(l.c_str());
	items.push_back(nullptr);
	ui->im.choice = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(items.data()));
	gtk_widget_set_halign(GTK_WIDGET(ui->im.choice), GTK_ALIGN_START);
	g_signal_connect_swapped(ui->im.choice, "notify::selected", G_CALLBACK(on_im_choice), ui);
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(ui->im.choice));

	ui->im.details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	{
		/* Pointing at whichever framework is chosen (update_im_configure). */
		static const wb::Integration none;
		ui->im.configure = configure_button(&none);
		gtk_widget_set_visible(ui->im.configure, FALSE);
		gtk_box_append(GTK_BOX(ui->im.details), ui->im.configure);
	}
	ui->im.command = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(ui->im.command, _("Command, e.g. fcitx5 --replace"));
	gtk_widget_set_size_request(GTK_WIDGET(ui->im.command), 360, -1);
	gtk_widget_set_halign(GTK_WIDGET(ui->im.command), GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(ui->im.details), GTK_WIDGET(ui->im.command));
	GtkWidget *label = gtk_label_new(_("Variables for programs (NAME=value, one per line):"));
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(ui->im.details), label);
	GtkWidget *frame = gtk_frame_new(nullptr);
	GtkWidget *tv = gtk_text_view_new();
	gtk_widget_set_size_request(tv, 360, 80);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
	gtk_frame_set_child(GTK_FRAME(frame), tv);
	gtk_widget_set_halign(frame, GTK_ALIGN_START);
	ui->im.env = GTK_TEXT_VIEW(tv);
	gtk_box_append(GTK_BOX(ui->im.details), frame);
	gtk_widget_set_visible(ui->im.details, FALSE);
	gtk_box_append(GTK_BOX(box), ui->im.details);
	return box;
}

void populate_input_method(Ui *ui, const std::optional<wb::InputMethodSetting> &im) {
	if (ui->im.choice == nullptr)
		return;
	ui->im.updating = true;
	guint idx = 0;
	if (im) {
		idx = static_cast<guint>(ui->im.frameworks.size() + 1);  /* Custom */
		/* Its exact command first (a wrapper such as "sh -c ..."), then by
		 * the program it runs. */
		for (size_t i = 0; i < ui->im.frameworks.size() && idx > ui->im.frameworks.size(); ++i)
			if (im->command == ui->im.frameworks[i]->exec)
				idx = static_cast<guint>(i + 1);
		for (size_t i = 0; i < ui->im.frameworks.size() && idx > ui->im.frameworks.size(); ++i)
			if (wb::startup_name("", im->command) == ui->im.frameworks[i]->binary)
				idx = static_cast<guint>(i + 1);
		gtk_editable_set_text(GTK_EDITABLE(ui->im.command), im->command.c_str());
		set_env_text(ui, env_text(im->env));
	} else {
		gtk_editable_set_text(GTK_EDITABLE(ui->im.command), "");
		set_env_text(ui, "");
	}
	gtk_drop_down_set_selected(ui->im.choice, idx);
	gtk_widget_set_visible(ui->im.details, idx != 0);
	ui->im.updating = false;
}

std::optional<wb::InputMethodSetting> collect_input_method(Ui *ui) {
	if (ui->im.choice == nullptr || gtk_drop_down_get_selected(ui->im.choice) == 0)
		return std::nullopt;
	wb::InputMethodSetting im;
	im.command = gtk_editable_get_text(GTK_EDITABLE(ui->im.command));
	if (im.command.empty())
		return std::nullopt;
	GtkTextBuffer *b = gtk_text_view_get_buffer(ui->im.env);
	GtkTextIter s, e;
	gtk_text_buffer_get_bounds(b, &s, &e);
	char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
	std::istringstream lines(text);
	g_free(text);
	std::string line;
	while (std::getline(lines, line)) {
		size_t eq = line.find('=');
		size_t start = line.find_first_not_of(" \t");
		if (eq == std::string::npos || start == std::string::npos || eq <= start)
			continue;
		std::string name = line.substr(start, eq - start);
		while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
			name.pop_back();
		im.env.emplace_back(name, line.substr(eq + 1));
	}
	return im;
}

void choose_input_method(Ui *ui, const std::string &id) {
	if (ui->im.choice == nullptr)
		return;
	guint idx = 0;
	if (id == "custom")
		idx = static_cast<guint>(ui->im.frameworks.size() + 1);
	for (size_t i = 0; i < ui->im.frameworks.size(); ++i)
		if (ui->im.frameworks[i]->id == id)
			idx = static_cast<guint>(i + 1);
	gtk_drop_down_set_selected(ui->im.choice, idx);
}

}  // namespace wbconf
