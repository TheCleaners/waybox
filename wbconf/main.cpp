/*
 * wbconf — a small GTK4 settings GUI for waybox, modelled after Openbox's
 * obconf (in scope and in visual layout: bold section headers over indented
 * content, a scrolling theme list, a sidebar of pages). It edits the user's
 * rc.xml in place (preserving keybindings, application rules and comments)
 * through wb::RcDocument, then asks a running waybox to reload (SIGUSR2). Only
 * the settings the compositor honours are exposed, plus the waybox extensions
 * (titlebar / menu / switcher).
 *
 * All user-visible strings are wrapped in _() for translation, consistent with
 * the rest of the project's gettext catalogue (text domain "waybox").
 */
#include "wbconf_ui.hpp"

namespace wbconf {

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

	/* Re-read the file (it may have changed since wbconf opened it) and
	 * write only what was changed here. */
	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	if (!doc) {
		set_status(ui, problem);
		return;
	}
	wb::WayboxSettings now = collect(ui);
	doc->apply(now, ui->shown);
	std::string err;
	if (!doc->save_file(path, &err)) {
		set_status(ui, std::string(_("Not saved: ")) + err);
		return;
	}
	ui->shown = now;
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

void on_reset_page(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int page = gtk_notebook_get_current_page(ui->notebook);
	reset_page_widgets(ui, page);
	set_status(ui, _("Reset this page to defaults (not yet saved)."));
}

void on_reset_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	gint pages = gtk_notebook_get_n_pages(ui->notebook);
	for (gint i = 0; i < pages; i++)
		reset_page_widgets(ui, i);
	set_status(ui, _("Reset all settings to defaults (not yet saved)."));
}

/* Closing with unsaved changes asks first. */
gboolean on_close_request(GtkWindow *window, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (!ui->loaded || collect(ui) == ui->shown)
		return FALSE;  /* close */
	GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", _("Discard unsaved changes?"));
	const char *buttons[] = {_("Cancel"), _("Discard"), nullptr};
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, window, nullptr,
			[](GObject *source, GAsyncResult *result, gpointer data) {
		auto *ui = static_cast<Ui *>(data);
		int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result,
				nullptr);
		if (button == 1) {
			ui->shown = collect(ui);  /* nothing left to lose */
			gtk_window_destroy(ui->window);
		}
	}, ui);
	g_object_unref(dialog);
	return TRUE;  /* keep it open until answered */
}

void add_tab(GtkWidget *notebook, GtkWidget *page, const char *label) {
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new(label));
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Configuration Manager"));
	gtk_window_set_default_size(ui->window, 640, 560);

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
	/* A sidebar: twelve pages don't fit side by side. */
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_LEFT);
	gtk_box_append(GTK_BOX(vbox), notebook);
	ui->notebook = GTK_NOTEBOOK(notebook);

	add_tab(notebook, build_theme_page(ui), _("Theme"));
	add_tab(notebook, build_appearance_page(ui), _("Appearance"));
	add_tab(notebook, build_fonts_page(ui), _("Fonts"));
	add_tab(notebook, build_windows_page(ui), _("Windows"));
	add_tab(notebook, build_margins_page(ui), _("Margins"));
	add_tab(notebook, build_menu_page(ui), _("Menu"));
	add_tab(notebook, build_switcher_page(ui), _("Switcher"));
	add_tab(notebook, build_desktops_page(ui), _("Desktops"));
	add_tab(notebook, build_monitors_page(ui), _("Monitors"));
	add_tab(notebook, build_keyboard_page(ui), _("Keyboard"));
	add_tab(notebook, build_mouse_page(ui), _("Mouse"));
	add_tab(notebook, build_advanced_page(ui), _("Advanced"));
	add_tab(notebook, build_integrations_page(ui), _("Integrations"));

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

	/* Reset: a split button — the main part resets the current page, the arrow
	 * opens a menu with "Reset This Page" / "Reset All to Defaults". GTK core
	 * has no split-button widget, so compose a linked button + menu button. */
	GtkWidget *reset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(reset_box, "linked");

	GtkWidget *reset_primary = gtk_button_new_with_label(_("Reset Page"));
	g_signal_connect(reset_primary, "clicked", G_CALLBACK(on_reset_page), ui);
	gtk_box_append(GTK_BOX(reset_box), reset_primary);

	GtkWidget *reset_menu = gtk_menu_button_new();
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_margin_top(pbox, 4);
	gtk_widget_set_margin_bottom(pbox, 4);
	gtk_widget_set_margin_start(pbox, 4);
	gtk_widget_set_margin_end(pbox, 4);

	GtkWidget *rp = gtk_button_new_with_label(_("Reset This Page"));
	gtk_button_set_has_frame(GTK_BUTTON(rp), FALSE);
	gtk_widget_set_halign(rp, GTK_ALIGN_FILL);
	g_signal_connect(rp, "clicked", G_CALLBACK(on_reset_page), ui);
	g_signal_connect_swapped(rp, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	gtk_box_append(GTK_BOX(pbox), rp);

	GtkWidget *ra = gtk_button_new_with_label(_("Reset All to Defaults"));
	gtk_button_set_has_frame(GTK_BUTTON(ra), FALSE);
	gtk_widget_set_halign(ra, GTK_ALIGN_FILL);
	g_signal_connect(ra, "clicked", G_CALLBACK(on_reset_all), ui);
	g_signal_connect_swapped(ra, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	gtk_box_append(GTK_BOX(pbox), ra);

	gtk_popover_set_child(GTK_POPOVER(pop), pbox);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(reset_menu), pop);
	gtk_box_append(GTK_BOX(reset_box), reset_menu);
	gtk_box_append(GTK_BOX(actions), reset_box);

	GtkWidget *close = gtk_button_new_with_label(_("Close"));
	g_signal_connect(close, "clicked", G_CALLBACK(on_close), ui);
	gtk_box_append(GTK_BOX(actions), close);

	GtkWidget *save = gtk_button_new_with_label(_("Save & Apply"));
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), ui);
	gtk_box_append(GTK_BOX(actions), save);
	ui->save = save;

	gtk_box_append(GTK_BOX(vbox), actions);

	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	populate(ui, doc ? doc->read() : wb::WayboxSettings{});
	ui->loaded = doc.has_value();
	if (!ui->loaded) {
		/* A full-width banner above the tabs; Save stays off. */
		gtk_widget_set_sensitive(save, FALSE);
		GtkWidget *banner = gtk_label_new(problem.c_str());
		gtk_label_set_wrap(GTK_LABEL(banner), TRUE);
		gtk_label_set_xalign(GTK_LABEL(banner), 0);
		gtk_widget_add_css_class(banner, "error");
		gtk_widget_set_margin_top(banner, 8);
		gtk_widget_set_margin_start(banner, 12);
		gtk_widget_set_margin_end(banner, 12);
		gtk_box_prepend(GTK_BOX(vbox), banner);
	}
	g_signal_connect(ui->window, "close-request", G_CALLBACK(on_close_request), ui);
	/* WBCONF_PAGE=N opens on that page (screenshots, tests). */
	if (const char *page = getenv("WBCONF_PAGE"))
		gtk_notebook_set_current_page(ui->notebook, atoi(page));
	gtk_window_present(ui->window);
}

void on_activate(GtkApplication *app, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	/* wbconf runs once: launching it again brings the open window to the
	 * front (building a second one over the same state broke the first). */
	if (ui->window != nullptr) {
		gtk_window_present(ui->window);
		return;
	}
	build_window(app, ui);
}

}  // namespace wbconf

using namespace wbconf;

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
