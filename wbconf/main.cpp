/*
 * wbconf: Waybox's settings editor. Its settings are described by a schema
 * (data/wbconf/settings.json, plus any *.json in ~/.config/waybox/wbconf)
 * and shown as one searchable list with a table of contents, in the manner
 * of VS Code's settings editor (settings_view.cpp); the theme, monitors and
 * startup programs have editors of their own. It edits the user's rc.xml in
 * place, writing only what changed (keybindings, application rules and
 * comments stay as they are), then asks a running Waybox to reload.
 *
 * All user-visible strings are wrapped in _() for translation (text domain
 * "waybox").
 */
#include "wbconf_ui.hpp"

namespace wbconf {

void set_status(Ui *ui, const std::string &text) {
	if (ui->status != nullptr)
		gtk_label_set_text(ui->status, text.c_str());
}

namespace {

bool unsaved(Ui *ui) {
	return ui->loaded && (rows_unsaved(ui) || collect(ui) != ui->shown);
}

}  // namespace

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
	for (auto &r : ui->rows)
		if (r->def->type != wb::SettingType::Custom && r->value != r->saved)
			doc->set(r->def->rc, r->value);
	wb::WayboxSettings now = collect(ui);
	doc->apply(now, ui->shown);
	std::string err;
	if (!doc->save_file(path, &err)) {
		set_status(ui, std::string(_("Not saved: ")) + err);
		return;
	}
	ui->shown = now;
	rows_saved(ui);
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

/* Every setting back to its default (not saved until Save). */
void on_reset_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	reset_all_rows(ui);
	gtk_list_box_unselect_all(ui->theme_list);
	populate_monitors(ui, {});
	for (auto &[name, check] : ui->hdr)
		gtk_check_button_set_active(check, FALSE);
	ui->hdr_others.clear();
	for (auto &row : ui->programs)
		gtk_switch_set_active(row->on, FALSE);
	preview_refresh(ui);
	set_status(ui, _("Reset all settings to defaults (not yet saved)."));
}

gboolean on_close_request(GtkWindow *window, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (!unsaved(ui))
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
			ui->loaded = false;  /* nothing left to lose */
			gtk_window_destroy(ui->window);
		}
	}, ui);
	g_object_unref(dialog);
	return TRUE;  /* keep it open until answered */
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Settings"));
	gtk_window_set_default_size(ui->window, 980, 720);

	/* An explicit header bar: GTK then owns a proper, draggable client-side
	 * titlebar that drives compositor move/maximize correctly (GTK doesn't
	 * implement server-side decorations). The search box sits in it. */
	GtkWidget *header = gtk_header_bar_new();
	gtk_window_set_titlebar(ui->window, header);
	GtkWidget *search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
			_("Search settings (@modified, @id:...)"));
	gtk_widget_set_size_request(search, 420, -1);
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), search);
	ui->search = GTK_SEARCH_ENTRY(search);
	/* Typing anywhere searches, as in VS Code. */
	gtk_search_entry_set_key_capture_widget(ui->search, GTK_WIDGET(ui->window));

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(ui->window, vbox);
	gtk_box_append(GTK_BOX(vbox), build_settings_view(ui));
	gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

	/* Action row + status line. */
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(actions, 12);
	gtk_widget_set_margin_end(actions, 12);
	gtk_widget_set_margin_top(actions, 8);
	gtk_widget_set_margin_bottom(actions, 8);

	GtkWidget *status = gtk_label_new("");
	gtk_widget_set_hexpand(status, TRUE);
	gtk_widget_set_halign(status, GTK_ALIGN_START);
	gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
	ui->status = GTK_LABEL(status);
	gtk_box_append(GTK_BOX(actions), status);

	GtkWidget *reset = gtk_button_new_with_label(_("Reset All to Defaults"));
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_all), ui);
	gtk_box_append(GTK_BOX(actions), reset);

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
	populate(ui, doc ? doc->read() : wb::WayboxSettings{}, doc ? &*doc : nullptr);
	ui->loaded = doc.has_value();
	if (!ui->loaded) {
		/* A full-width banner above the list; Save stays off. */
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
	/* WBCONF_CATEGORY=id opens at a category, WBCONF_SEARCH=text with a
	 * search (screenshots, tests). */
	if (const char *q = getenv("WBCONF_SEARCH"))
		gtk_editable_set_text(GTK_EDITABLE(ui->search), q);
	if (const char *cat = getenv("WBCONF_CATEGORY")) {
		g_object_set_data_full(G_OBJECT(ui->window), "wb-category", g_strdup(cat), g_free);
		g_timeout_add_once(300, [](gpointer data) {
			auto *ui = static_cast<Ui *>(data);
			focus_category(ui, static_cast<const char *>(
					g_object_get_data(G_OBJECT(ui->window), "wb-category")));
		}, ui);
	}
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
