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
	if (sel >= 0) {
		GtkListBoxRow *selected = gtk_list_box_get_row_at_index(ui->theme_list, sel);
		gtk_list_box_select_row(ui->theme_list, selected);
		/* Scroll it into view once the list is laid out (focusing a row does). */
		g_idle_add_once([](gpointer row) { gtk_widget_grab_focus(GTK_WIDGET(row)); },
				selected);
	}

	populate_monitors(ui, s.outputs);

	ui->hdr_loaded = s.hdr_outputs;
	ui->hdr_others.clear();
	std::vector<std::string> hdr = split_names(s.hdr_outputs.value_or(""));
	for (const std::string &n : hdr)
		if (ui->hdr.count(n) == 0)
			ui->hdr_others.push_back(n);
	for (auto &[name, check] : ui->hdr)
		gtk_check_button_set_active(check,
				std::find(hdr.begin(), hdr.end(), name) != hdr.end());

	populate_programs(ui, s.startup);
	populate_rows(ui, doc);

	ui->shown = collect(ui);
	preview_refresh(ui);
}

/* What the custom editors hold now. */
wb::WayboxSettings collect(Ui *ui) {
	wb::WayboxSettings s;
	if (GtkListBoxRow *r = gtk_list_box_get_selected_row(ui->theme_list)) {
		int idx = gtk_list_box_row_get_index(r);
		if (idx >= 0 && idx < static_cast<int>(ui->theme_names.size()))
			s.theme_name = ui->theme_names[idx];
	}

	for (Ui::Monitor &m : ui->monitors) {
		wb::OutputSetting out{m.name, m.kept};
		if (!gtk_switch_get_active(m.enabled))
			out.attrs["enabled"] = "no";
		auto put = [&out](const char *key, const std::string &v) {
			if (!v.empty())
				out.attrs[key] = v;
		};
		put("mode", selected_value(m.mode, m.modes));
		put("scale", selected_value(m.scale, m.scales));
		put("position", gtk_editable_get_text(GTK_EDITABLE(m.position)));
		put("transform", selected_value(m.transform, kTransforms));
		if (!out.attrs.empty())
			s.outputs.push_back(out);
	}
	for (const wb::OutputSetting &o : ui->other_outputs)
		s.outputs.push_back(o);

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
	return s;
}

}  // namespace wbconf
