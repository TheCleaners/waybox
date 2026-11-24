#include "wbconf_ui.hpp"

namespace wbconf {

GtkWidget *build_fonts_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Fonts"));
	GtkWidget *grid = section_grid(content);
	GCallback refresh = G_CALLBACK(preview_refresh);
	add_font(grid, 0, _("Active window title:"), &ui->font_active, refresh, ui);
	add_font(grid, 1, _("Inactive window title:"), &ui->font_inactive, refresh, ui);
	add_font(grid, 2, _("Menu header:"), &ui->font_menu_header, refresh, ui);
	add_font(grid, 3, _("Menu item:"), &ui->font_menu_item, refresh, ui);
	add_font(grid, 4, _("On-screen display:"), &ui->font_osd, refresh, ui);
	return page;
}

GtkWidget *build_appearance_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Window Titles"));
	GtkWidget *grid = section_grid(content);
	ui->pad_y = add_spin(grid, 0, _("Titlebar padding (px):"), 0, 16);
	ui->button_size = add_spin(grid, 1, _("Button size (px, 0 = auto):"), 0, 64);
	ui->resize_grab = add_spin(grid, 2, _("Resize grab margin (px):"), 0, 32);

	/* As obconf's "Button order". */
	grid_label(grid, 3, _("Button order:"));
	ui->title_layout = GTK_ENTRY(grid_control(grid, 3, gtk_entry_new()));
	gtk_entry_set_max_length(ui->title_layout, 16);
	GtkWidget *legend = gtk_label_new(_("N: Window icon\n"
			"L: Window label (title)\n"
			"I: Iconify\n"
			"M: Maximize\n"
			"C: Close\n"
			"S: Shade (roll up)\n"
			"D: Omnipresent (on all desktops)"));
	gtk_label_set_xalign(GTK_LABEL(legend), 0);
	gtk_widget_add_css_class(legend, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), legend, 1, 4, 1, 1);
	return page;
}

GtkWidget *build_windows_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Placing Windows"));
	GtkWidget *grid = section_grid(content);
	ui->placement = add_dropdown(grid, 0, _("Prefer to place windows:"),
			kPlacement);

	GtkWidget *tiling = section_grid(section(page, _("Edge Tiling")));
	ui->edge_tiling = add_switch(tiling, 0, _("Tile windows dragged to a screen edge:"));
	ui->edge_range = add_spin(tiling, 1, _("Distance from the edge (px):"), 1, 100);
	return page;
}

GtkWidget *build_desktops_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("Desktops")));
	ui->desktops_number = add_spin(grid, 0, _("Number of desktops:"), 1, 64);
	ui->popup_time = add_spin(grid, 1, _("Show the name after switching (ms, 0 = off):"),
			0, 10000);
	gtk_spin_button_set_increments(ui->popup_time, 125, 500);

	GtkWidget *names = section(page, _("Names (one per line)"));
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 120);
	gtk_widget_set_vexpand(scroller, TRUE);
	GtkWidget *view = gtk_text_view_new();
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 4);
	ui->desktop_names = GTK_TEXT_VIEW(view);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	gtk_box_append(GTK_BOX(names), scroller);
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
	ui->switcher_icons = add_switch(grid, 3, _("Show window icons:"));
	return page;
}

GtkWidget *build_keyboard_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("Keyboard Layout")));
	struct Field { const char *key, *label, *hint; };
	const Field fields[] = {
		{"layout", _("Layouts:"), _("us  (several: us,de)")},
		{"variant", _("Variants:"), _("e.g. dvorak  (one per layout)")},
		{"options", _("Options:"), _("e.g. grp:alt_shift_toggle,caps:escape")},
		{"model", _("Model:"), _("e.g. pc105")},
	};
	int row = 0;
	for (const Field &f : fields) {
		grid_label(grid, row, f.label);
		GtkWidget *entry = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(entry), f.hint);
		gtk_widget_set_size_request(entry, 280, -1);
		grid_control(grid, row++, entry);
		ui->xkb[f.key] = GTK_ENTRY(entry);
	}
	GtkWidget *note = gtk_label_new(_("Empty fields use the system default. "
			"Options and layouts are XKB names, as listed in xkeyboard-config(7)."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);
	return page;
}

GtkWidget *build_mouse_page(Ui *ui) {
	GtkWidget *page = make_page();
	const std::vector<std::pair<std::string, std::string>> onoff = {
		{"", _("Default")}, {"enabled", _("On")}, {"disabled", _("Off")}};
	auto add = [ui](GtkWidget *grid, int row, const char *key, const char *label,
			const std::vector<std::pair<std::string, std::string>> &items) {
		grid_label(grid, row, label);
		Ui::Choice c;
		c.dd = choice_dropdown(items, c.values);
		grid_control(grid, row, GTK_WIDGET(c.dd));
		ui->libinput[key] = c;
	};
	std::vector<std::pair<std::string, std::string>> speeds = {{"", _("Default")}};
	for (int i = -4; i <= 4; i++) {
		char v[16];
		snprintf(v, sizeof(v), "%g", i / 4.0);
		speeds.push_back({v, i == 0 ? std::string("0") : std::string(v)});
	}

	GtkWidget *pointer = section_grid(section(page, _("Mouse and Touchpad")));
	add(pointer, 0, "accelProfile", _("Acceleration:"),
			{{"", _("Default")}, {"adaptive", _("Adaptive")}, {"flat", _("Flat")}});
	add(pointer, 1, "accelSpeed", _("Speed (-1 slow to 1 fast):"), speeds);
	add(pointer, 2, "naturalScroll", _("Natural scrolling:"), onoff);
	add(pointer, 3, "leftHanded", _("Left-handed:"), onoff);
	add(pointer, 4, "middleEmulation", _("Middle click by pressing both buttons:"), onoff);

	GtkWidget *touch = section_grid(section(page, _("Touchpad")));
	add(touch, 0, "tap", _("Tap to click:"), onoff);
	add(touch, 1, "tapDrag", _("Tap and drag:"), onoff);
	add(touch, 2, "tapDragLock", _("Drag lock:"), onoff);
	add(touch, 3, "disableWhileTyping", _("Disable while typing:"), onoff);
	add(touch, 4, "scrollMethod", _("Scrolling:"),
			{{"", _("Default")}, {"twofinger", _("Two fingers")}, {"edge", _("Edge")},
			 {"button", _("Holding a button")}, {"none", _("None")}});
	add(touch, 5, "clickMethod", _("Clicking:"),
			{{"", _("Default")}, {"buttonAreas", _("Button areas")},
			 {"clickfinger", _("Number of fingers")}, {"none", _("None")}});

	GtkWidget *note = gtk_label_new(_("Applied to every mouse and touchpad; a "
			"device ignores what it doesn't support. Default is the device's own."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page);
	gtk_widget_set_vexpand(scroller, TRUE);
	return scroller;
}

GtkWidget *build_advanced_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("X11 Programs")));
	ui->xwayland = add_switch(grid, 0, _("Run X11 programs (Xwayland):"));
	GtkWidget *vgrid = section_grid(section(page, _("Virtual Keyboards and Pointers")));
	grid_label(vgrid, 0, _("Accept their input:"));
	ui->virtual_input.dd = choice_dropdown(
			{{"unlocked", _("Unless the screen is locked (default)")},
			 {"always", _("Always")}, {"never", _("Never")}},
			ui->virtual_input.values);
	grid_control(vgrid, 0, GTK_WIDGET(ui->virtual_input.dd));
	GtkWidget *note = gtk_label_new(_("Virtual input comes from tools such as wtype, "
			"on-screen keyboards and remote-desktop programs."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);
	return page;
}

}  // namespace wbconf
