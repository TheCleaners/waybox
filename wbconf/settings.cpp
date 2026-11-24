#include "wbconf_ui.hpp"

namespace wbconf {

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
	if (sel >= 0) {
		GtkListBoxRow *selected = gtk_list_box_get_row_at_index(ui->theme_list, sel);
		gtk_list_box_select_row(ui->theme_list, selected);
		/* Scroll it into view once the list is laid out (focusing a row does). */
		g_idle_add_once([](gpointer row) { gtk_widget_grab_focus(GTK_WIDGET(row)); },
				selected);
	}

	gtk_spin_button_set_value(ui->pad_y, s.titlebar_pad_y.value_or(2));
	gtk_spin_button_set_value(ui->button_size, s.titlebar_button_size.value_or(0));
	gtk_spin_button_set_value(ui->resize_grab, s.titlebar_resize_grab.value_or(8));
	gtk_editable_set_text(GTK_EDITABLE(ui->title_layout),
			s.title_layout.value_or("NLIMC").c_str());

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
	gtk_switch_set_active(ui->switcher_icons, s.switcher_icons.value_or(true));

	/* Always show a concrete font (the configured one, else waybox's effective
	 * default Sans 10) so the picker opens with the current font selected and
	 * the user can just tweak the size. */
	font_set(&ui->font_active, s.font_active_window.value_or("Sans 10"));
	font_set(&ui->font_inactive, s.font_inactive_window.value_or("Sans 10"));
	font_set(&ui->font_menu_header, s.font_menu_header.value_or("Sans 10"));
	font_set(&ui->font_menu_item, s.font_menu_item.value_or("Sans 10"));
	font_set(&ui->font_osd, s.font_osd.value_or("Sans 10"));

	gtk_spin_button_set_value(ui->desktops_number, s.desktops_number.value_or(4));
	std::string names;
	for (const std::string &n : s.desktop_names.value_or(std::vector<std::string>{}))
		names += n + "\n";
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(ui->desktop_names),
			names.c_str(), -1);
	gtk_spin_button_set_value(ui->popup_time, s.desktop_popup_time.value_or(875));

	gtk_switch_set_active(ui->edge_tiling, s.edge_tiling.value_or(true));
	gtk_spin_button_set_value(ui->edge_range, s.edge_tiling_range.value_or(10));

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

	for (auto &[key, entry] : ui->xkb) {
		auto it = s.xkb.find(key);
		gtk_editable_set_text(GTK_EDITABLE(entry), it == s.xkb.end() ? "" : it->second.c_str());
	}
	for (auto &[key, choice] : ui->libinput) {
		auto it = s.libinput.find(key);
		select_value(choice.dd, choice.values, it == s.libinput.end() ? "" : it->second);
	}
	gtk_switch_set_active(ui->xwayland, s.xwayland.value_or(true));
	select_value(ui->virtual_input.dd, ui->virtual_input.values,
			s.virtual_input.value_or("unlocked"));
	populate_programs(ui, s.startup);
	gtk_spin_button_set_value(ui->screen_off,
			std::round(s.idle_screen_off.value_or(0) / 60.0));

	ui->shown = collect(ui);
	preview_refresh(ui);
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
	{
		/* Only Openbox's letters, upper case. */
		std::string layout;
		for (char c : std::string(gtk_editable_get_text(GTK_EDITABLE(ui->title_layout)))) {
			char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if (std::string("NLIMCSD").find(u) != std::string::npos)
				layout += u;
		}
		/* Nothing usable left: the default, rather than a titlebar with no
		 * buttons at all. */
		s.title_layout = layout.empty() ? std::string("NLIMC") : layout;
	}

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
	s.switcher_icons = gtk_switch_get_active(ui->switcher_icons);

	s.font_active_window = font_value(&ui->font_active);
	s.font_inactive_window = font_value(&ui->font_inactive);
	s.font_menu_header = font_value(&ui->font_menu_header);
	s.font_menu_item = font_value(&ui->font_menu_item);
	s.font_osd = font_value(&ui->font_osd);

	s.desktops_number = gtk_spin_button_get_value_as_int(ui->desktops_number);
	GtkTextBuffer *buf = gtk_text_view_get_buffer(ui->desktop_names);
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buf, &start, &end);
	gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
	std::vector<std::string> names;
	std::istringstream lines(text);
	for (std::string line; std::getline(lines, line);)
		names.push_back(line);
	g_free(text);
	while (!names.empty() && names.back().empty())
		names.pop_back();
	if (!names.empty())
		s.desktop_names = names;
	s.desktop_popup_time = gtk_spin_button_get_value_as_int(ui->popup_time);

	s.edge_tiling = gtk_switch_get_active(ui->edge_tiling);
	s.edge_tiling_range = gtk_spin_button_get_value_as_int(ui->edge_range);

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

	for (auto &[key, entry] : ui->xkb) {
		std::string v = gtk_editable_get_text(GTK_EDITABLE(entry));
		if (!v.empty())
			s.xkb[key] = v;
	}
	for (auto &[key, choice] : ui->libinput) {
		std::string v = selected_value(choice.dd, choice.values);
		if (!v.empty())
			s.libinput[key] = v;
	}
	s.xwayland = gtk_switch_get_active(ui->xwayland);
	s.virtual_input = selected_value(ui->virtual_input.dd, ui->virtual_input.values);
	s.startup = collect_programs(ui);
	if (int minutes = gtk_spin_button_get_value_as_int(ui->screen_off); minutes > 0)
		s.idle_screen_off = minutes * 60;
	return s;
}

/* ---- Reset to defaults ----------------------------------------------- */

/* The default values for one notebook page, set straight onto its widgets.
 * Page order matches build_window: 0 Theme, 1 Appearance, 2 Fonts, 3 Windows,
 * 4 Margins, 5 Menu, 6 Switcher, 7 Desktops, 8 Monitors, 9 Keyboard,
 * 10 Mouse and touchpad, 11 Advanced. */
void reset_page_widgets(Ui *ui, int page) {
	switch (page) {
	case 0:  /* Theme: clear the selection (built-in default theme). */
		gtk_list_box_unselect_all(ui->theme_list);
		break;
	case 1:  /* Appearance */
		gtk_spin_button_set_value(ui->pad_y, 2);
		gtk_spin_button_set_value(ui->button_size, 0);
		gtk_spin_button_set_value(ui->resize_grab, 8);
		gtk_editable_set_text(GTK_EDITABLE(ui->title_layout), "NLIMC");
		break;
	case 2:  /* Fonts */
		font_set(&ui->font_active, "Sans 10");
		font_set(&ui->font_inactive, "Sans 10");
		font_set(&ui->font_menu_header, "Sans 10");
		font_set(&ui->font_menu_item, "Sans 10");
		font_set(&ui->font_osd, "Sans 10");
		break;
	case 3:  /* Windows */
		gtk_drop_down_set_selected(ui->placement, 0);  /* Smart */
		gtk_switch_set_active(ui->edge_tiling, TRUE);
		gtk_spin_button_set_value(ui->edge_range, 10);
		break;
	case 4:  /* Margins */
		gtk_spin_button_set_value(ui->margin_top, 0);
		gtk_spin_button_set_value(ui->margin_bottom, 0);
		gtk_spin_button_set_value(ui->margin_left, 0);
		gtk_spin_button_set_value(ui->margin_right, 0);
		break;
	case 5:  /* Menu */
		gtk_editable_set_text(GTK_EDITABLE(ui->menu_source), "builtin");
		gtk_drop_down_set_selected(ui->submenu_open, 0);  /* hover */
		gtk_spin_button_set_value(ui->hover_delay, 100);
		gtk_switch_set_active(ui->menu_wrap, TRUE);
		gtk_switch_set_active(ui->menu_icons, TRUE);
		break;
	case 6:  /* Switcher */
		gtk_drop_down_set_selected(ui->switcher_order, 0);  /* mru */
		gtk_switch_set_active(ui->switcher_osd, TRUE);
		gtk_switch_set_active(ui->switcher_wrap, TRUE);
		gtk_switch_set_active(ui->switcher_icons, TRUE);
		break;
	case 7:  /* Desktops */
		gtk_spin_button_set_value(ui->desktops_number, 4);
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(ui->desktop_names), "", -1);
		gtk_spin_button_set_value(ui->popup_time, 875);
		break;
	case 9:  /* Keyboard */
		for (auto &[key, entry] : ui->xkb)
			gtk_editable_set_text(GTK_EDITABLE(entry), "");
		break;
	case 10:  /* Mouse and touchpad */
		for (auto &[key, choice] : ui->libinput)
			gtk_drop_down_set_selected(choice.dd, 0);
		break;
	case 11:  /* Advanced */
		gtk_switch_set_active(ui->xwayland, TRUE);
		gtk_drop_down_set_selected(ui->virtual_input.dd, 0);
		break;
	case 12:  /* Integrations: nothing started, screens never off */
		for (auto &row : ui->programs)
			gtk_switch_set_active(row->on, FALSE);
		gtk_spin_button_set_value(ui->screen_off, 0);
		break;
	case 8:  /* Monitors: everything automatic, no HDR */
		for (auto &[name, check] : ui->hdr)
			gtk_check_button_set_active(check, FALSE);
		for (Ui::Monitor &m : ui->monitors) {
			gtk_switch_set_active(m.enabled, TRUE);
			gtk_drop_down_set_selected(m.mode, 0);
			gtk_drop_down_set_selected(m.scale, 0);
			gtk_editable_set_text(GTK_EDITABLE(m.position), "");
			gtk_drop_down_set_selected(m.transform, 0);
		}
		break;
	}
	preview_refresh(ui);
}

}  // namespace wbconf
