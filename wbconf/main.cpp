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
#include "version.h"

namespace wbconf {

void set_status(Ui *ui, const std::string &text) {
	if (ui->status != nullptr) {
		gtk_label_set_text(ui->status, text.c_str());
		g_object_set_data(G_OBJECT(ui->status), "wb-unsaved-status", nullptr);
	}
}

namespace {

/* How many settings differ from rc.xml: rows, plus each custom editor. */
int unsaved_count(Ui *ui) {
	if (!ui->loaded)
		return 0;
	int n = 0;
	for (auto &r : ui->rows)
		if (r->value != r->saved)
			++n;
	wb::WayboxSettings now = collect(ui);
	n += now.theme_name != ui->shown.theme_name;
	n += now.outputs != ui->shown.outputs || now.hdr_outputs != ui->shown.hdr_outputs;
	n += now.startup != ui->shown.startup;
	n += now.input_method != ui->shown.input_method;
	n += bindings_unsaved(ui);
	n += sources_unsaved(ui);
	return n;
}

bool unsaved(Ui *ui) {
	return unsaved_count(ui) > 0;
}

const char *kUnsavedTag = "wb-unsaved-status";

}  // namespace

void refresh_save_state(Ui *ui) {
	if (ui->save == nullptr || ui->status == nullptr)
		return;
	int n = unsaved_count(ui);
	gtk_widget_set_sensitive(ui->save, n > 0);
	bool showing = g_object_get_data(G_OBJECT(ui->status), kUnsavedTag) != nullptr;
	if (n > 0) {
		char *t = g_strdup_printf(ngettext("%d change not saved yet",
				"%d changes not saved yet", n), n);
		set_status(ui, t);
		g_free(t);
		g_object_set_data(G_OBJECT(ui->status), kUnsavedTag, GINT_TO_POINTER(1));
	} else if (showing) {
		set_status(ui, "");
		g_object_set_data(G_OBJECT(ui->status), kUnsavedTag, nullptr);
	}
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
	for (auto &r : ui->rows)
		if (r->def->type != wb::SettingType::Custom && r->value != r->saved)
			doc->set(r->def->rc, r->value);
	wb::WayboxSettings now = collect(ui);
	doc->apply(now, ui->shown);
	apply_bindings(ui, *doc);
	std::string err;
	if (!doc->save_file(path, &err)) {
		set_status(ui, std::string(_("Not saved: ")) + err);
		return;
	}
	ui->shown = now;
	rows_saved(ui);
	bindings_saved(ui);
	if (!save_sources(ui, &err)) {
		refresh_save_state(ui);
		set_status(ui, std::string(_("Saved rc.xml, but not the autostart changes: ")) + err);
		return;
	}
	sources_saved(ui);
	refresh_save_state(ui);
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
	for (auto &b : ui->bindings)
		gtk_switch_set_active(b->on, FALSE);
	choose_input_method(ui, "");
	preview_refresh(ui);
	refresh_save_state(ui);
}

namespace {

/* Read rc.xml again, dropping changes not saved. */
void reload_from_file(Ui *ui) {
	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	if (!doc) {
		set_status(ui, problem);
		return;
	}
	populate(ui, doc->read(), &*doc);
	refresh_save_state(ui);
	set_status(ui, _("Reloaded from rc.xml."));
}

void on_reload(GtkButton *, gpointer data) {
	reload_from_file(static_cast<Ui *>(data));
}

/* Open rc.xml in the default text editor (VS Code's "Open Settings
 * (JSON)"). */
void on_open_rc(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string path = user_rc_path();
	if (path.empty() || !fs::exists(path)) {
		set_status(ui, _("There's no rc.xml of your own yet: Save creates it."));
		return;
	}
	GFile *file = g_file_new_for_path(path.c_str());
	GtkFileLauncher *launcher = gtk_file_launcher_new(file);
	gtk_file_launcher_launch(launcher, ui->window, nullptr, nullptr, nullptr);
	g_object_unref(launcher);
	g_object_unref(file);
}

gboolean on_shortcut_save(GtkWidget *, GVariant *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (gtk_widget_get_sensitive(ui->save))
		on_save(nullptr, ui);
	return TRUE;
}

gboolean on_shortcut_find(GtkWidget *, GVariant *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	gtk_widget_grab_focus(GTK_WIDGET(ui->search));
	return TRUE;
}

GtkWidget *menu_button_item(const char *label, GCallback cb, Ui *ui, GtkWidget *pop) {
	GtkWidget *b = gtk_button_new_with_label(label);
	gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
	gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
	g_signal_connect(b, "clicked", cb, ui);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	return b;
}

}  // namespace

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
	gtk_window_set_default_size(ui->window, 880, 620);

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

	/* Open rc.xml, and a menu for the rest. */
	GtkWidget *menu = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
	gtk_widget_set_tooltip_text(menu, _("Menu"));
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(pbox), menu_button_item(_("Reload from rc.xml"),
			G_CALLBACK(on_reload), ui, pop));
	gtk_box_append(GTK_BOX(pbox), menu_button_item(_("Reset All to Defaults"),
			G_CALLBACK(on_reset_all), ui, pop));
	/* Which builds these are (git describe), for bug reports. */
	gtk_box_append(GTK_BOX(pbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	std::string running = running_waybox_version();
	std::string about = std::string(_("wbconf ")) + WB_BUILD_VERSION + "\n" +
			(running.empty() ? std::string(_("Waybox: not known (not started from Waybox)"))
			                 : std::string(_("Waybox (running) ")) + running);
	GtkWidget *versions = gtk_label_new(about.c_str());
	gtk_label_set_xalign(GTK_LABEL(versions), 0);
	gtk_label_set_selectable(GTK_LABEL(versions), TRUE);
	gtk_widget_add_css_class(versions, "dim-label");
	gtk_widget_set_margin_start(versions, 8);
	gtk_widget_set_margin_end(versions, 8);
	gtk_widget_set_margin_top(versions, 4);
	gtk_widget_set_margin_bottom(versions, 4);
	gtk_box_append(GTK_BOX(pbox), versions);
	gtk_popover_set_child(GTK_POPOVER(pop), pbox);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), pop);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu);
	/* Text, not an icon: clearer, and not at the mercy of icon themes. */
	GtkWidget *open_rc = gtk_button_new_with_label(_("Edit rc.xml"));
	gtk_widget_set_tooltip_text(open_rc, _("Open rc.xml in a text editor"));
	g_signal_connect(open_rc, "clicked", G_CALLBACK(on_open_rc), ui);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), open_rc);

	/* Ctrl+S saves, Ctrl+F searches (Esc in the search clears it). */
	GtkEventController *keys = gtk_shortcut_controller_new();
	gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys), GTK_SHORTCUT_SCOPE_GLOBAL);
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(
			gtk_shortcut_trigger_parse_string("<Control>s"),
			gtk_callback_action_new(on_shortcut_save, ui, nullptr)));
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(
			gtk_shortcut_trigger_parse_string("<Control>f"),
			gtk_callback_action_new(on_shortcut_find, ui, nullptr)));
	gtk_widget_add_controller(GTK_WIDGET(ui->window), keys);

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
	if (const char *set = getenv("WBCONF_TEST_SET"))
		apply_test_values(ui, set);
	refresh_save_state(ui);
	/* The custom editors (theme, monitors, programs) change in many ways:
	 * check now and then rather than wire every widget. */
	g_timeout_add(400, [](gpointer data) -> gboolean {
		auto *ui = static_cast<Ui *>(data);
		if (ui->window == nullptr)
			return G_SOURCE_REMOVE;
		refresh_save_state(ui);
		return G_SOURCE_CONTINUE;
	}, ui);
	g_signal_connect_swapped(ui->window, "destroy", G_CALLBACK(+[](Ui *ui) {
		ui->window = nullptr;
	}), ui);
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
