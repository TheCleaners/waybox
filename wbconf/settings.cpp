#include "wbconf_ui.hpp"

namespace wbconf {

/* ---- The custom editors: theme, monitors, programs ------------------ */

/* (The other settings are rows from the schema: settings_view.cpp.) */
void populate(Ui *ui, const wb::WayboxSettings &s, const wb::RcDocument *doc) {
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
	ui->theme_thumbs.clear();
	for (size_t i = 0; i < ui->theme_names.size(); ++i) {
		/* The name over a thumbnail of the theme's titlebars. */
		GtkWidget *item = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_margin_top(item, 4);
		gtk_widget_set_margin_bottom(item, 4);
		gtk_widget_set_margin_start(item, 6);
		gtk_widget_set_margin_end(item, 6);
		GtkWidget *label = gtk_label_new(ui->theme_names[i].c_str());
		gtk_widget_set_halign(label, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(item), label);
		GtkWidget *thumb = gtk_drawing_area_new();
		gtk_widget_set_size_request(thumb, 220, 52);
		g_object_set_data_full(G_OBJECT(thumb), "wb-theme",
				g_strdup(ui->theme_names[i].c_str()), g_free);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(thumb), thumb_draw, ui, nullptr);
		gtk_box_append(GTK_BOX(item), thumb);
		ui->theme_thumbs.push_back(thumb);
		gtk_list_box_append(ui->theme_list, item);
		if (ui->theme_names[i] == current)
			sel = static_cast<int>(i);
	}
	if (sel >= 0) {
		GtkListBoxRow *selected = gtk_list_box_get_row_at_index(ui->theme_list, sel);
		gtk_list_box_select_row(ui->theme_list, selected);
		/* Scroll it into view within the theme list once laid out (not
		 * by focusing it, which scrolled the whole settings list too). */
		/* Retried until the window is shown and laid out (a few tries). */
		g_object_set_data(G_OBJECT(selected), "wb-tries", GINT_TO_POINTER(0));
		g_timeout_add(100, [](gpointer data) -> gboolean {
			GtkWidget *row = GTK_WIDGET(data);
			int tries = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "wb-tries"));
			g_object_set_data(G_OBJECT(row), "wb-tries", GINT_TO_POINTER(tries + 1));
			GtkWidget *sw = gtk_widget_get_ancestor(row, GTK_TYPE_SCROLLED_WINDOW);
			GtkWidget *list = gtk_widget_get_parent(row);
			graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), out;
			GtkAdjustment *adj = sw ? gtk_scrolled_window_get_vadjustment(
					GTK_SCROLLED_WINDOW(sw)) : nullptr;
			if (adj == nullptr || list == nullptr || !gtk_widget_get_mapped(row) ||
					gtk_adjustment_get_page_size(adj) <= 0 ||
					!gtk_widget_compute_point(row, list, &p, &out))
				return tries < 50 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
			double page = gtk_adjustment_get_page_size(adj);
			gtk_adjustment_set_value(adj, out.y - page / 2 + gtk_widget_get_height(row) / 2.0);
			return G_SOURCE_REMOVE;
		}, selected);
	}

	populate_monitors(ui, s.outputs);
	populate_hdr(ui, s.hdr_outputs);
	/* The pointer: rc.xml's <cursor> over GTK's own value. */
	ui->gtk.rc_cursor_theme = s.cursor_theme;
	ui->gtk.rc_cursor_size = s.cursor_size;
	if (ui->gtk.cursor != nullptr && s.cursor_theme && !s.cursor_theme->empty()) {
		select_value(ui->gtk.cursor, ui->gtk.cursor_values, *s.cursor_theme);
		ui->gtk.saved.cursor = *s.cursor_theme;
	}
	if (ui->gtk.size != nullptr && s.cursor_size && atoi(s.cursor_size->c_str()) >= 8) {
		gtk_spin_button_set_value(ui->gtk.size, atoi(s.cursor_size->c_str()));
		ui->gtk.saved.size = atoi(s.cursor_size->c_str());
	}

	populate_programs(ui, s.startup);
	populate_bindings(ui, doc);
	populate_input_method(ui, s.input_method);
	populate_rows(ui, doc);

	ui->shown = collect(ui);
	preview_refresh(ui);
}

/* What the custom editors hold now. */
void populate_hdr(Ui *ui, const std::optional<std::string> &hdr_outputs) {
	ui->hdr_loaded = hdr_outputs;
	ui->hdr_others.clear();
	std::vector<std::string> hdr = split_names(hdr_outputs.value_or(""));
	for (const std::string &n : hdr)
		if (ui->hdr.count(n) == 0)
			ui->hdr_others.push_back(n);
	for (auto &[name, check] : ui->hdr)
		gtk_check_button_set_active(check,
				std::find(hdr.begin(), hdr.end(), name) != hdr.end());
}

wb::WayboxSettings collect(Ui *ui) {
	wb::WayboxSettings s;
	if (GtkListBoxRow *r = gtk_list_box_get_selected_row(ui->theme_list)) {
		int idx = gtk_list_box_row_get_index(r);
		if (idx >= 0 && idx < static_cast<int>(ui->theme_names.size()))
			s.theme_name = ui->theme_names[idx];
	}

	for (Ui::Monitor &m : ui->monitors)
		if (auto rule = monitor_rule(m))
			s.outputs.push_back(*rule);
	for (const wb::OutputSetting &o : ui->other_outputs)
		s.outputs.push_back(o);

	/* The pointer controls (Appearance > Applications) write <cursor> too:
	 * Waybox draws the pointer for most programs. */
	s.cursor_theme = ui->gtk.rc_cursor_theme;
	s.cursor_size = ui->gtk.rc_cursor_size;
	if (ui->gtk.cursor != nullptr) {
		std::string theme = selected_value(ui->gtk.cursor, ui->gtk.cursor_values);
		if (!theme.empty())
			s.cursor_theme = theme;
		s.cursor_size = std::to_string(gtk_spin_button_get_value_as_int(ui->gtk.size));
	}

	std::vector<std::string> hdr;
	for (const Ui::Monitor &m : ui->monitors)
		if (ui->hdr.count(m.name) && gtk_check_button_get_active(ui->hdr.at(m.name)))
			hdr.push_back(m.name);
	for (const std::string &n : ui->hdr_others)
		hdr.push_back(n);
	std::vector<std::string> loaded = split_names(ui->hdr_loaded.value_or(""));
	std::vector<std::string> a = hdr, b = loaded;
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	if (a == b) {
		s.hdr_outputs = ui->hdr_loaded;  /* unchanged: as written */
	} else if (!hdr.empty()) {
		std::string joined;
		for (const std::string &n : hdr)
			joined += (joined.empty() ? "" : " ") + n;
		s.hdr_outputs = joined;
	}

	s.startup = collect_programs(ui);
	s.input_method = collect_input_method(ui);
	return s;
}

}  // namespace wbconf
