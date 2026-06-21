/*
 * wbconf — a small GTK4 settings GUI for waybox, modelled after Openbox's
 * obconf (in scope and in visual layout: bold section headers over indented
 * content, a scrolling theme list, obconf-style tabs). It edits the user's
 * rc.xml in place (preserving keybindings, application rules and comments)
 * through wb::RcDocument, then asks a running waybox to reload (SIGUSR2). Only
 * the settings the compositor honours are exposed, plus the waybox extensions
 * (titlebar / menu / switcher).
 *
 * All user-visible strings are wrapped in _() for translation, consistent with
 * the rest of the project's gettext catalogue (text domain "waybox").
 */
#include <gtk/gtk.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>
#include <libintl.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/wbconf.hpp"

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "waybox"
#endif
#define _(s) gettext(s)

namespace {

namespace fs = std::filesystem;

/* A blank rc.xml used only when the user has no config and none is installed. */
const char *kSkeletonRc =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"</openbox_config>\n";

std::string home_dir() {
	if (const char *h = getenv("HOME"); h && h[0])
		return h;
	return {};
}

/* Where wbconf writes: $XDG_CONFIG_HOME/waybox/rc.xml, else ~/.config/...  */
std::string user_rc_path() {
	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
		return (fs::path(xdg) / "waybox" / "rc.xml").string();
	std::string h = home_dir();
	if (!h.empty())
		return (fs::path(h) / ".config" / "waybox" / "rc.xml").string();
	return {};
}

/* The system default, used to seed a brand-new user config. */
std::string system_rc_path() {
#ifdef WB_SYSCONFDIR
	return (fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "rc.xml").string();
#else
	return {};
#endif
}

/* Load the document to edit: the user's file if present, else the system
 * default (so the user inherits its settings), else a blank skeleton. */
wb::RcDocument load_document() {
	std::error_code ec;
	std::string up = user_rc_path();
	if (!up.empty() && fs::exists(up, ec))
		if (auto d = wb::RcDocument::load_file(up))
			return std::move(*d);
	std::string sp = system_rc_path();
	if (!sp.empty() && fs::exists(sp, ec))
		if (auto d = wb::RcDocument::load_file(sp))
			return std::move(*d);
	return *wb::RcDocument::from_string(kSkeletonRc);
}

/* Ask every waybox process owned by this user to reconfigure (SIGUSR2), the
 * same signal Openbox uses. Returns the number signalled. */
int signal_waybox_reload() {
	int signalled = 0;
	uid_t me = getuid();
	DIR *proc = opendir("/proc");
	if (proc == nullptr)
		return 0;
	struct dirent *e;
	while ((e = readdir(proc)) != nullptr) {
		char *end = nullptr;
		long pid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0' || pid <= 0)
			continue;
		std::string comm_path = std::string("/proc/") + e->d_name + "/comm";
		std::ifstream comm(comm_path);
		std::string name;
		if (!std::getline(comm, name) || name != "waybox")
			continue;
		struct stat st {};
		std::string dir = std::string("/proc/") + e->d_name;
		if (stat(dir.c_str(), &st) != 0 || st.st_uid != me)
			continue;
		if (kill(static_cast<pid_t>(pid), SIGUSR2) == 0)
			++signalled;
	}
	closedir(proc);
	return signalled;
}

/* ---- Widget set, bound to a WayboxSettings on save ------------------- */

struct Ui {
	GtkWindow *window = nullptr;
	GtkLabel *status = nullptr;

	/* Appearance */
	GtkListBox *theme_list = nullptr;        /* obconf-style scrolling list */
	std::vector<std::string> theme_names;    /* parallel to the list rows */
	GtkSpinButton *pad_y = nullptr;
	GtkSpinButton *button_size = nullptr;
	GtkSpinButton *resize_grab = nullptr;

	/* Windows */
	GtkDropDown *placement = nullptr;  /* Smart / Center / UnderMouse */

	/* Margins */
	GtkSpinButton *margin_top = nullptr;
	GtkSpinButton *margin_bottom = nullptr;
	GtkSpinButton *margin_left = nullptr;
	GtkSpinButton *margin_right = nullptr;

	/* Menu */
	GtkEntry *menu_source = nullptr;
	GtkDropDown *submenu_open = nullptr;  /* hover / click */
	GtkSpinButton *hover_delay = nullptr;
	GtkSwitch *menu_wrap = nullptr;
	GtkSwitch *menu_icons = nullptr;

	/* Switcher */
	GtkDropDown *switcher_order = nullptr;  /* mru / stacking / spatial */
	GtkSwitch *switcher_osd = nullptr;
	GtkSwitch *switcher_wrap = nullptr;
};

const char *const kPlacement[] = {"Smart", "Center", "UnderMouse", nullptr};
const char *const kSubmenu[] = {"hover", "click", nullptr};
const char *const kOrder[] = {"mru", "stacking", "spatial", nullptr};

int index_of(const char *const *list, const std::string &value, int fallback) {
	for (int i = 0; list[i] != nullptr; ++i)
		if (value == list[i])
			return i;
	return fallback;
}

/* ---- obconf-style layout helpers ------------------------------------- */

/* A notebook page: a vertical box with obconf's outer padding. */
GtkWidget *make_page() {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(page, 12);
	gtk_widget_set_margin_bottom(page, 12);
	gtk_widget_set_margin_start(page, 12);
	gtk_widget_set_margin_end(page, 12);
	return page;
}

/* A bold section header followed by an indented content box (obconf's idiom of
 * "<span weight=\"bold\">Title</span>" over a 12px-indented VBox). Returns the
 * content box to which callers append their controls. */
GtkWidget *section(GtkWidget *page, const char *title) {
	GtkWidget *header = gtk_label_new(nullptr);
	char *markup = g_markup_printf_escaped("<span weight=\"bold\">%s</span>",
			title);
	gtk_label_set_markup(GTK_LABEL(header), markup);
	g_free(markup);
	gtk_widget_set_halign(header, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), header);

	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(content, 12);
	gtk_box_append(GTK_BOX(page), content);
	return content;
}

/* A grid for label/control rows, appended to a section's content box. */
GtkWidget *section_grid(GtkWidget *content) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_box_append(GTK_BOX(content), grid);
	return grid;
}

void grid_label(GtkWidget *grid, int row, const char *text) {
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
}

GtkWidget *grid_control(GtkWidget *grid, int row, GtkWidget *w) {
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
	return w;
}

GtkSpinButton *add_spin(GtkWidget *grid, int row, const char *label,
		int min, int max) {
	grid_label(grid, row, label);
	GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);
	grid_control(grid, row, spin);
	return GTK_SPIN_BUTTON(spin);
}

GtkSwitch *add_switch(GtkWidget *grid, int row, const char *label) {
	grid_label(grid, row, label);
	GtkWidget *sw = gtk_switch_new();
	gtk_widget_set_halign(sw, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), sw, 1, row, 1, 1);
	return GTK_SWITCH(sw);
}

GtkDropDown *add_dropdown(GtkWidget *grid, int row, const char *label,
		const char *const *items) {
	grid_label(grid, row, label);
	GtkWidget *dd = gtk_drop_down_new_from_strings(items);
	grid_control(grid, row, dd);
	return GTK_DROP_DOWN(dd);
}

/* ---- Read settings into the widgets ---------------------------------- */

void populate(Ui *ui, const wb::WayboxSettings &s) {
	/* Theme list: every installed theme, current one guaranteed present and
	 * selected (obconf shows the active theme highlighted in the list). */
	ui->theme_names = wb::installed_theme_names();
	std::string current = s.theme_name.value_or("");
	if (!current.empty() &&
			std::find(ui->theme_names.begin(), ui->theme_names.end(), current) ==
					ui->theme_names.end())
		ui->theme_names.insert(ui->theme_names.begin(), current);
	if (ui->theme_names.empty() && !current.empty())
		ui->theme_names.push_back(current);

	/* Clear any existing rows, then repopulate. */
	GtkListBoxRow *row;
	while ((row = gtk_list_box_get_row_at_index(ui->theme_list, 0)) != nullptr)
		gtk_list_box_remove(ui->theme_list, GTK_WIDGET(row));
	int sel = -1;
	for (size_t i = 0; i < ui->theme_names.size(); ++i) {
		GtkWidget *label = gtk_label_new(ui->theme_names[i].c_str());
		gtk_widget_set_halign(label, GTK_ALIGN_START);
		gtk_widget_set_margin_top(label, 4);
		gtk_widget_set_margin_bottom(label, 4);
		gtk_widget_set_margin_start(label, 6);
		gtk_widget_set_margin_end(label, 6);
		gtk_list_box_append(ui->theme_list, label);
		if (ui->theme_names[i] == current)
			sel = static_cast<int>(i);
	}
	if (sel >= 0)
		gtk_list_box_select_row(ui->theme_list,
				gtk_list_box_get_row_at_index(ui->theme_list, sel));

	gtk_spin_button_set_value(ui->pad_y, s.titlebar_pad_y.value_or(2));
	gtk_spin_button_set_value(ui->button_size, s.titlebar_button_size.value_or(0));
	gtk_spin_button_set_value(ui->resize_grab, s.titlebar_resize_grab.value_or(8));

	gtk_drop_down_set_selected(ui->placement,
			index_of(kPlacement, s.placement_policy.value_or("Smart"), 0));

	gtk_spin_button_set_value(ui->margin_top, s.margin_top.value_or(0));
	gtk_spin_button_set_value(ui->margin_bottom, s.margin_bottom.value_or(0));
	gtk_spin_button_set_value(ui->margin_left, s.margin_left.value_or(0));
	gtk_spin_button_set_value(ui->margin_right, s.margin_right.value_or(0));

	gtk_editable_set_text(GTK_EDITABLE(ui->menu_source),
			s.menu_source.value_or("builtin").c_str());
	gtk_drop_down_set_selected(ui->submenu_open,
			index_of(kSubmenu, s.menu_submenu_open.value_or("hover"), 0));
	gtk_spin_button_set_value(ui->hover_delay, s.menu_hover_delay.value_or(100));
	gtk_switch_set_active(ui->menu_wrap, s.menu_wrap.value_or(true));
	gtk_switch_set_active(ui->menu_icons, s.menu_icons.value_or(true));

	gtk_drop_down_set_selected(ui->switcher_order,
			index_of(kOrder, s.switcher_order.value_or("mru"), 0));
	gtk_switch_set_active(ui->switcher_osd, s.switcher_osd.value_or(true));
	gtk_switch_set_active(ui->switcher_wrap, s.switcher_wrap.value_or(true));
}

/* ---- Collect widget state back into a WayboxSettings ----------------- */

wb::WayboxSettings collect(Ui *ui) {
	wb::WayboxSettings s;
	if (GtkListBoxRow *r = gtk_list_box_get_selected_row(ui->theme_list)) {
		int idx = gtk_list_box_row_get_index(r);
		if (idx >= 0 && idx < static_cast<int>(ui->theme_names.size()))
			s.theme_name = ui->theme_names[idx];
	}
	s.titlebar_pad_y = gtk_spin_button_get_value_as_int(ui->pad_y);
	s.titlebar_button_size = gtk_spin_button_get_value_as_int(ui->button_size);
	s.titlebar_resize_grab = gtk_spin_button_get_value_as_int(ui->resize_grab);

	s.placement_policy = kPlacement[gtk_drop_down_get_selected(ui->placement)];

	s.margin_top = gtk_spin_button_get_value_as_int(ui->margin_top);
	s.margin_bottom = gtk_spin_button_get_value_as_int(ui->margin_bottom);
	s.margin_left = gtk_spin_button_get_value_as_int(ui->margin_left);
	s.margin_right = gtk_spin_button_get_value_as_int(ui->margin_right);

	s.menu_source = gtk_editable_get_text(GTK_EDITABLE(ui->menu_source));
	s.menu_submenu_open = kSubmenu[gtk_drop_down_get_selected(ui->submenu_open)];
	s.menu_hover_delay = gtk_spin_button_get_value_as_int(ui->hover_delay);
	s.menu_wrap = gtk_switch_get_active(ui->menu_wrap);
	s.menu_icons = gtk_switch_get_active(ui->menu_icons);

	s.switcher_order = kOrder[gtk_drop_down_get_selected(ui->switcher_order)];
	s.switcher_osd = gtk_switch_get_active(ui->switcher_osd);
	s.switcher_wrap = gtk_switch_get_active(ui->switcher_wrap);
	return s;
}

void set_status(Ui *ui, const std::string &text) {
	if (ui->status != nullptr)
		gtk_label_set_text(ui->status, text.c_str());
}

void on_save(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string path = user_rc_path();
	if (path.empty()) {
		set_status(ui, _("Could not determine the rc.xml path."));
		return;
	}
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);

	wb::RcDocument doc = load_document();
	doc.apply(collect(ui));
	if (!doc.save_file(path)) {
		set_status(ui, std::string(_("Failed to write ")) + path);
		return;
	}
	int n = signal_waybox_reload();
	if (n > 0)
		set_status(ui, _("Saved and reloaded waybox."));
	else
		set_status(ui, std::string(_("Saved to ")) + path + " " +
				_("(no running waybox to reload)."));
}

void on_close(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	gtk_window_close(ui->window);
}

/* ---- obconf-style tab pages ------------------------------------------ */

GtkWidget *build_theme_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Theme"));

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroller, TRUE);

	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
	ui->theme_list = GTK_LIST_BOX(list);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
	gtk_box_append(GTK_BOX(content), scroller);
	return page;
}

GtkWidget *build_appearance_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Window Titles"));
	GtkWidget *grid = section_grid(content);
	ui->pad_y = add_spin(grid, 0, _("Titlebar padding (px):"), 0, 16);
	ui->button_size = add_spin(grid, 1, _("Button size (px, 0 = auto):"), 0, 64);
	ui->resize_grab = add_spin(grid, 2, _("Resize grab margin (px):"), 0, 32);
	return page;
}

GtkWidget *build_windows_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Placing Windows"));
	GtkWidget *grid = section_grid(content);
	ui->placement = add_dropdown(grid, 0, _("Prefer to place windows:"),
			kPlacement);
	return page;
}

GtkWidget *build_margins_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Reserved Screen Margins"));
	GtkWidget *grid = section_grid(content);
	ui->margin_top = add_spin(grid, 0, _("Top:"), 0, 1024);
	ui->margin_bottom = add_spin(grid, 1, _("Bottom:"), 0, 1024);
	ui->margin_left = add_spin(grid, 2, _("Left:"), 0, 1024);
	ui->margin_right = add_spin(grid, 3, _("Right:"), 0, 1024);
	return page;
}

GtkWidget *build_menu_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Root Menu"));
	GtkWidget *grid = section_grid(content);
	grid_label(grid, 0, _("Source (builtin or command):"));
	ui->menu_source = GTK_ENTRY(grid_control(grid, 0, gtk_entry_new()));
	ui->submenu_open = add_dropdown(grid, 1, _("Submenu opens on:"), kSubmenu);
	ui->hover_delay = add_spin(grid, 2, _("Submenu hover delay (ms):"), 0, 2000);
	ui->menu_wrap = add_switch(grid, 3, _("Wrap selection:"));
	ui->menu_icons = add_switch(grid, 4, _("Show icons:"));
	return page;
}

GtkWidget *build_switcher_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Task Switcher"));
	GtkWidget *grid = section_grid(content);
	ui->switcher_order = add_dropdown(grid, 0, _("Order:"), kOrder);
	ui->switcher_osd = add_switch(grid, 1, _("Show on-screen display:"));
	ui->switcher_wrap = add_switch(grid, 2, _("Wrap selection:"));
	return page;
}

void add_tab(GtkWidget *notebook, GtkWidget *page, const char *label) {
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new(label));
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Configuration Manager"));
	gtk_window_set_default_size(ui->window, 500, 460);

	/* An explicit header bar: GTK then owns a proper, draggable client-side
	 * titlebar (a GtkWindowHandle) that drives compositor move/maximize
	 * correctly. Without one, a decorated GtkWindow falls back to a degenerate
	 * titlebar that misbehaves under a wlroots compositor (the window can jump
	 * or resize on drag). waybox leaves GTK windows client-side decorated
	 * because GTK does not implement the server-side xdg-decoration protocol. */
	GtkWidget *header = gtk_header_bar_new();
	gtk_window_set_titlebar(ui->window, header);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_window_set_child(ui->window, vbox);

	GtkWidget *notebook = gtk_notebook_new();
	gtk_widget_set_vexpand(notebook, TRUE);
	gtk_box_append(GTK_BOX(vbox), notebook);

	add_tab(notebook, build_theme_page(ui), _("Theme"));
	add_tab(notebook, build_appearance_page(ui), _("Appearance"));
	add_tab(notebook, build_windows_page(ui), _("Windows"));
	add_tab(notebook, build_margins_page(ui), _("Margins"));
	add_tab(notebook, build_menu_page(ui), _("Menu"));
	add_tab(notebook, build_switcher_page(ui), _("Switcher"));

	/* Action row + status line. */
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(actions, 12);
	gtk_widget_set_margin_end(actions, 12);
	gtk_widget_set_margin_bottom(actions, 12);

	GtkWidget *status = gtk_label_new("");
	gtk_widget_set_hexpand(status, TRUE);
	gtk_widget_set_halign(status, GTK_ALIGN_START);
	ui->status = GTK_LABEL(status);
	gtk_box_append(GTK_BOX(actions), status);

	GtkWidget *close = gtk_button_new_with_label(_("Close"));
	g_signal_connect(close, "clicked", G_CALLBACK(on_close), ui);
	gtk_box_append(GTK_BOX(actions), close);

	GtkWidget *save = gtk_button_new_with_label(_("Save & Apply"));
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), ui);
	gtk_box_append(GTK_BOX(actions), save);

	gtk_box_append(GTK_BOX(vbox), actions);

	populate(ui, load_document().read());
	gtk_window_present(ui->window);
}

void on_activate(GtkApplication *app, gpointer data) {
	build_window(app, static_cast<Ui *>(data));
}

}  // namespace

int main(int argc, char **argv) {
	setlocale(LC_ALL, "");
#ifdef LOCALEDIR
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
#endif
	textdomain(GETTEXT_PACKAGE);

	Ui ui;
	GtkApplication *app = gtk_application_new("org.waybox.wbconf",
			G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
