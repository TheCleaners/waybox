/*
 * The settings list, generated from the schema (data/wbconf/settings.json),
 * in the manner of VS Code's settings editor: a search box, a table of
 * contents, and one scrolling list of settings, each with its category and
 * title, description, control, a bar marking it as set in rc.xml (or not
 * saved yet) and a menu to reset it or copy it.
 */
#include "wbconf_ui.hpp"

#include "waybox/monitor_layout.hpp"

namespace wbconf {

namespace {

const char *kCss = R"(
.wb-setting { padding: 3px 8px 5px 0; border-radius: 6px; }
.wb-setting:hover { background: alpha(currentColor, 0.04); }
.wb-setting .wb-gear { opacity: 0; }
.wb-setting:hover .wb-gear, .wb-gear:checked { opacity: 1; }
.wb-bar { min-width: 3px; margin-right: 12px; border-radius: 2px; }
.wb-bar.wb-set { background: @theme_selected_bg_color; }
.wb-bar.wb-unsaved { background: #e5a50a; }
.wb-category { font-size: 1.25em; font-weight: bold; margin-top: 14px; margin-bottom: 0; }
.wb-section { font-weight: bold; margin-top: 8px; margin-bottom: 1px; }
.wb-toc row { padding: 1px 4px; min-height: 0; }
.wb-toc row label { font-size: 0.95em; }
.wb-toc .wb-toc-arrow { opacity: 0.6; -gtk-icon-size: 12px; }
.wb-toc .wb-toc-section { margin-left: 18px; }
.wb-keycap { padding: 0 7px; min-height: 22px; border-radius: 5px; font-size: 0.92em;
  border: 1px solid alpha(currentColor, 0.22); border-bottom-width: 2px;
  background-color: alpha(currentColor, 0.05); }
.wb-shortcut-row { padding: 4px 6px; margin: 0; min-height: 0; border-radius: 6px; font-weight: normal; }
.wb-shortcut-static { padding: 4px 6px; }
.wb-shortcut-open > box > .wb-shortcut-row { background-color: alpha(currentColor, 0.06); }
.wb-shortcut-unsaved > box > .wb-shortcut-row { background-color: alpha(@theme_selected_bg_color, 0.14); }
.wb-removed label { text-decoration-line: line-through; opacity: 0.55; }
.wb-shortcut-editor { margin: 2px 0 10px 6px; padding: 12px 14px; border-radius: 8px;
  background-color: alpha(currentColor, 0.035); border: 1px solid alpha(currentColor, 0.10); }
.wb-keys-button { padding: 4px 8px; min-height: 30px; }
.wb-remove { color: mix(@error_color, @theme_fg_color, 0.3); }
.wb-mod-toggle { padding: 2px 10px; min-height: 26px; font-size: 0.95em; }
.wb-keys-button.wb-capturing { outline: 2px solid @theme_selected_bg_color; outline-offset: 1px;
  background-color: alpha(@theme_selected_bg_color, 0.16); }
.wb-shortcut-group { font-weight: bold; margin-top: 10px; margin-bottom: 2px; }
.wb-shortcut-place { font-weight: bold; }
.wb-toc-toolbar button { min-width: 22px; min-height: 22px; padding: 0; margin: 0;
  border-radius: 5px; opacity: 0.55; -gtk-icon-size: 14px; }
.wb-toc-toolbar button:hover, .wb-toc-toolbar button:checked { opacity: 1; }
.wb-toc-toolbar menubutton > button { min-width: 22px; min-height: 22px; padding: 0; }
.wb-problem-icon { color: @warning_color; }
.wb-toc .wb-problem-icon { -gtk-icon-size: 12px; }
.wb-problem { background: alpha(@warning_color, 0.12); border-radius: 6px; padding: 6px 8px; }
.wb-approval { background: alpha(currentColor, 0.05); border: 1px solid alpha(currentColor, 0.12);
               border-radius: 8px; padding: 10px 12px; margin: 2px 0 6px 0; }
.wb-font-preview { background: alpha(@theme_selected_bg_color, 0.12);
                   border: 1px solid alpha(@theme_selected_bg_color, 0.5);
                   border-radius: 8px; padding: 14px 16px; }
.wb-changed { background: alpha(@theme_selected_bg_color, 0.14); border-radius: 6px;
              padding: 6px 8px; }
.wb-script { border: 1px solid alpha(currentColor, 0.12); border-radius: 6px; }
.wb-script textview, .wb-script text { background: alpha(currentColor, 0.03); }
)";

/* The title in bold (with `crumb`, after its section or category, dimmed:
 * "Pointer: Natural scrolling"). The headings above give the context, in
 * search results too, so rows don't repeat it. */
std::string title_markup(const Ui::SettingRow &r, bool crumb) {
	const std::string &where = r.def->section.empty() ? r.category : r.def->section;
	char *m = crumb && where != r.def->title
		? g_markup_printf_escaped("<span alpha=\"65%%\">%s: </span><b>%s</b>",
				where.c_str(), r.def->title.c_str())
		: g_markup_printf_escaped("<b>%s</b>", r.def->title.c_str());
	std::string out = m;
	g_free(m);
	return out;
}

/* The rows sharing one line (a group, or just one setting). */
std::vector<Ui::SettingRow *> line_of(Ui::SettingRow *r) {
	std::vector<Ui::SettingRow *> out;
	Ui *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(r->row), "wb-ui"));
	if (ui == nullptr)
		return {r};
	for (auto &o : ui->rows)
		if (o->row == r->row)
			out.push_back(o.get());
	if (out.empty())
		out.push_back(r);
	return out;
}

void refresh_marker(Ui::SettingRow *r) {
	if (r->bar == nullptr)
		return;
	gtk_widget_remove_css_class(r->bar, "wb-set");
	gtk_widget_remove_css_class(r->bar, "wb-unsaved");
	bool set = false, unsaved = false;
	for (Ui::SettingRow *o : line_of(r)) {
		set = set || o->value.has_value();
		unsaved = unsaved || o->value != o->saved;
	}
	if (unsaved) {
		gtk_widget_add_css_class(r->bar, "wb-unsaved");
		gtk_widget_set_tooltip_text(r->bar, _("Changed, not saved yet"));
	} else if (set) {
		gtk_widget_add_css_class(r->bar, "wb-set");
		gtk_widget_set_tooltip_text(r->bar, _("Set in rc.xml"));
	} else {
		gtk_widget_set_tooltip_text(r->bar, nullptr);
	}
}

void update_status(Ui *ui) {
	refresh_save_state(ui);
}

/* Put a row's value (or its default) into its control. */
void show_value(Ui *ui, Ui::SettingRow *r) {
	const wb::SettingDef &d = *r->def;
	std::string v = r->value.value_or(d.def);
	r->updating = true;
	switch (d.type) {
	case wb::SettingType::Bool:
		gtk_check_button_set_active(r->check, wb::setting_true(v));
		break;
	case wb::SettingType::Int:
		gtk_spin_button_set_value(r->spin, atof(v.c_str()) / d.scale);
		break;
	case wb::SettingType::Enum: {
		guint idx = 0;
		bool found = false;
		for (size_t i = 0; i < d.choices.size(); ++i)
			if (d.choices[i].value == v) {
				idx = static_cast<guint>(i);
				found = true;
			}
		if (!found && !v.empty()) {
			/* A value rc.xml has that the schema doesn't list: shown as is. */
			GtkStringList *model = GTK_STRING_LIST(gtk_drop_down_get_model(r->dropdown));
			guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
			if (n == d.choices.size())
				gtk_string_list_append(model, v.c_str());
			idx = static_cast<guint>(d.choices.size());
		}
		gtk_drop_down_set_selected(r->dropdown, idx);
		break;
	}
	case wb::SettingType::String:
	case wb::SettingType::Color:  /* its color control's field */
		gtk_editable_set_text(GTK_EDITABLE(r->entry), r->value.value_or("").c_str());
		break;
	case wb::SettingType::Font:
		/* Not set: "Default (Sans 10)", the font Waybox uses then. */
		font_control_set(r->font.get(), r->value.value_or(""));
		break;
	case wb::SettingType::Lines:
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(r->text),
				r->value.value_or("").c_str(), -1);
		break;
	case wb::SettingType::Custom:
		break;
	}
	r->updating = false;
	refresh_marker(r);
	(void)ui;
}

/* Read a row's control back into its value. A value equal to the default,
 * for a setting rc.xml doesn't have, stays unset (trying a switch and
 * putting it back doesn't add it to the file). */
void read_value(Ui *ui, Ui::SettingRow *r) {
	if (r->updating)
		return;
	const wb::SettingDef &d = *r->def;
	std::optional<std::string> v;
	switch (d.type) {
	case wb::SettingType::Bool:
		v = gtk_check_button_get_active(r->check) ? "yes" : "no";
		if (!d.def.empty() && wb::setting_true(*v) == wb::setting_true(d.def) && !r->saved)
			v.reset();
		break;
	case wb::SettingType::Int:
		v = std::to_string(gtk_spin_button_get_value_as_int(r->spin) * d.scale);
		if (*v == d.def && !r->saved)
			v.reset();
		break;
	case wb::SettingType::Enum: {
		guint idx = gtk_drop_down_get_selected(r->dropdown);
		if (idx < d.choices.size())
			v = d.choices[idx].value;
		else if (r->value)
			v = r->value;  /* the unlisted value, unchanged */
		if (v && (v->empty() || (*v == d.def && !r->saved)))
			v.reset();
		break;
	}
	case wb::SettingType::String:
	case wb::SettingType::Color: {
		std::string t = gtk_editable_get_text(GTK_EDITABLE(r->entry));
		if (!t.empty())
			v = t;
		break;
	}
	case wb::SettingType::Font:
		if (!r->font->value.empty())
			v = r->font->value;
		if (v && *v == (d.def.empty() ? "Sans 10" : d.def) && !r->saved)
			v.reset();
		break;
	case wb::SettingType::Lines: {
		GtkTextBuffer *b = gtk_text_view_get_buffer(r->text);
		GtkTextIter s, e;
		gtk_text_buffer_get_bounds(b, &s, &e);
		char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
		std::string t = text;
		g_free(text);
		while (!t.empty() && (t.back() == '\n' || t.back() == ' '))
			t.pop_back();
		if (!t.empty())
			v = t;
		break;
	}
	case wb::SettingType::Custom:
		return;
	}
	r->value = v;
	refresh_marker(r);
	update_status(ui);
	preview_refresh(ui);  /* fonts, button order, titlebar sizes */
}

void on_changed(Ui::SettingRow *r) {
	read_value(static_cast<Ui *>(g_object_get_data(G_OBJECT(r->row), "wb-ui")), r);
}

/* A preview request to Waybox: `request` with each setting on screen
 * (saved or not) as `key`; a setting not set, its default, or left out
 * when that's empty (unless `keep_empty` names it). nullptr: no answer. */
JsonObject *preview_request(Ui *ui, const char *request,
		std::initializer_list<std::pair<const char *, const char *>> keys,
		const char *keep_empty = nullptr) {
	JsonBuilder *jb = json_builder_new();
	json_builder_begin_object(jb);
	json_builder_set_member_name(jb, "request");
	json_builder_add_string_value(jb, request);
	for (const auto &[id, key] : keys)
		for (auto &r : ui->rows)
			if (r->def->id == id) {
				std::string v = r->value.value_or(r->def->def);
				if (v.empty() && (keep_empty == nullptr || strcmp(key, keep_empty) != 0))
					continue;
				json_builder_set_member_name(jb, key);
				json_builder_add_string_value(jb, v.c_str());
			}
	json_builder_end_object(jb);
	JsonGenerator *gen = json_generator_new();
	JsonNode *root = json_builder_get_root(jb);
	json_generator_set_root(gen, root);
	gchar *text = json_generator_to_data(gen, nullptr);
	JsonObject *reply = waybox_request(text);
	g_free(text);
	json_node_unref(root);
	g_object_unref(gen);
	g_object_unref(jb);
	return reply;
}

/* A Test button for a preview: greyed, saying why, without the socket. */
GtkWidget *preview_button(Ui *ui, void (*run)(Ui *)) {
	GtkWidget *b = gtk_button_new_with_mnemonic(_("_Test"));
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	JsonObject *probe = waybox_request("{\"request\":\"get_version\"}");
	if (probe != nullptr) {
		json_object_unref(probe);
	} else {
		gtk_widget_set_sensitive(b, FALSE);
		gtk_widget_set_tooltip_text(b, _("Needs Waybox's scripting socket: "
				"turned off in rc.xml (<ipc enabled=\"no\"/>)"));
	}
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(run), ui);
	return b;
}

/* Test: ring it with the settings on screen. */
void ring_bell_test(Ui *ui) {
	/* Not set: the default ("" for the command: none). */
	JsonObject *reply = preview_request(ui, "bell", {
		{"waybox.bell.flash", "flash"}, {"waybox.bell.color", "color"},
		{"waybox.bell.duration", "duration"}, {"waybox.bell.count", "count"},
		{"waybox.bell.width", "width"}, {"waybox.bell.command", "command"}}, "command");
	if (reply == nullptr) {
		set_status(ui, _("Waybox didn't answer: is its scripting socket on?"));
		return;
	}
	std::string shown = json_object_get_string_member_with_default(reply, "shown", "");
	json_object_unref(reply);
	if (shown == "paced")
		set_status(ui, _("Too soon after the last one: the bell never flashes more "
				"than 3 times a second."));
	else if (shown == "none")
		set_status(ui, _("Nothing to show: choose what the bell does."));
	else
		set_status(ui, "");
}

/* The bell with the settings on screen (saved or not): Waybox rings it as
 * the window in front would, without asking a script or telling anyone. */
GtkWidget *build_bell_test(Ui *ui) {
	return preview_button(ui, ring_bell_test);
}

/* Test: a sample on-screen display with the settings on screen. */
void show_osd_test(Ui *ui) {
	JsonObject *reply = preview_request(ui, "osd", {
		{"waybox.osd.position", "position"}, {"waybox.osd.opacity", "opacity"},
		{"waybox.osd.cornerRadius", "cornerRadius"}, {"waybox.osd.bg", "bg"},
		{"waybox.osd.text", "text"}, {"waybox.osd.border", "border"}});
	if (reply == nullptr) {
		set_status(ui, _("Waybox didn't answer: is its scripting socket on?"));
		return;
	}
	bool ok = json_object_get_boolean_member_with_default(reply, "ok", FALSE);
	std::string error = json_object_get_string_member_with_default(reply, "error", "");
	json_object_unref(reply);
	set_status(ui, ok ? "" : error);
}

GtkWidget *build_osd_test(Ui *ui) {
	return preview_button(ui, show_osd_test);
}

/* ---- The row's menu: reset, copy ---- */

void copy_text(GtkWidget *w, const std::string &text) {
	gdk_clipboard_set_text(gtk_widget_get_clipboard(w), text.c_str());
}

/* The setting as rc.xml would have it, without the rest of the file. */
std::string as_xml(const wb::SettingDef &d, const std::string &value) {
	auto doc = wb::RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"</openbox_config>\n");
	if (!doc)
		return {};
	doc->set(d.rc, value);
	std::string out = doc->to_string();
	size_t a = out.find('>', out.find("<openbox_config")) + 1;
	size_t b = out.rfind("</openbox_config>");
	if (a == std::string::npos || b == std::string::npos || b < a)
		return out;
	out = out.substr(a, b - a);
	while (!out.empty() && (out.front() == '\n' || out.front() == ' '))
		out.erase(0, 1);
	while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
		out.pop_back();
	return out;
}

GtkWidget *menu_item(const char *label, GCallback cb, Ui::SettingRow *r, GtkWidget *pop) {
	GtkWidget *b = gtk_button_new_with_label(label);
	gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
	gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
	g_signal_connect_swapped(b, "clicked", cb, r);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	return b;
}

void on_reset(Ui::SettingRow *r) {
	Ui *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(r->row), "wb-ui"));
	for (Ui::SettingRow *o : line_of(r)) {
		o->value.reset();
		show_value(ui, o);
		if (o->def->type == wb::SettingType::Font)
			preview_refresh(ui);
	}
	update_status(ui);
}

void on_copy_id(Ui::SettingRow *r) {
	std::string ids;
	for (Ui::SettingRow *o : line_of(r))
		ids += (ids.empty() ? "" : "\n") + o->def->id;
	copy_text(r->row, ids);
}

void on_copy_xml(Ui::SettingRow *r) {
	std::string xml;
	for (Ui::SettingRow *o : line_of(r))
		xml += (xml.empty() ? "" : "\n") + as_xml(*o->def, o->value.value_or(o->def->def));
	copy_text(r->row, xml);
}

GtkWidget *row_menu(Ui::SettingRow *r) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_widget_add_css_class(button, "wb-gear");  /* shown on hover */
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "emblem-system-symbolic");
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(button), FALSE);
	gtk_widget_set_tooltip_text(button, _("More actions"));
	gtk_widget_set_valign(button, GTK_ALIGN_START);
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(box), menu_item(_("Reset Setting"), G_CALLBACK(on_reset), r, pop));
	gtk_box_append(GTK_BOX(box), menu_item(_("Copy Setting ID"), G_CALLBACK(on_copy_id), r, pop));
	gtk_box_append(GTK_BOX(box), menu_item(_("Copy as rc.xml"), G_CALLBACK(on_copy_xml), r, pop));
	gtk_popover_set_child(GTK_POPOVER(pop), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), pop);
	return button;
}

/* ---- Building rows ---- */

GtkWidget *wrapped_label(const std::string &text, bool dim) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 90);
	if (dim)
		gtk_widget_add_css_class(l, "dim-label");
	return l;
}

GtkWidget *control_for(Ui *ui, Ui::SettingRow *r) {
	const wb::SettingDef &d = *r->def;
	switch (d.type) {
	case wb::SettingType::Bool: {
		/* The checkbox carries the title (VS Code style); the description,
		 * if any, goes below. */
		GtkWidget *c = gtk_check_button_new();
		GtkWidget *l = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(l), title_markup(*r, false).c_str());
		r->title = GTK_LABEL(l);
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_check_button_set_child(GTK_CHECK_BUTTON(c), l);
		r->check = GTK_CHECK_BUTTON(c);
		g_signal_connect_swapped(c, "toggled", G_CALLBACK(on_changed), r);
		return c;
	}
	case wb::SettingType::Int: {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		GtkWidget *spin = gtk_spin_button_new_with_range(d.min, d.max, 1);
		r->spin = GTK_SPIN_BUTTON(spin);
		g_signal_connect_swapped(spin, "value-changed", G_CALLBACK(on_changed), r);
		gtk_box_append(GTK_BOX(box), spin);
		if (!d.unit.empty())
			gtk_box_append(GTK_BOX(box), wrapped_label(d.unit, true));
		return box;
	}
	case wb::SettingType::Enum: {
		std::vector<const char *> items;
		for (const wb::SettingChoice &c : d.choices)
			items.push_back(c.label.c_str());
		items.push_back(nullptr);
		GtkWidget *dd = gtk_drop_down_new_from_strings(items.data());
		gtk_widget_set_halign(dd, GTK_ALIGN_START);
		r->dropdown = GTK_DROP_DOWN(dd);
		g_signal_connect_swapped(dd, "notify::selected", G_CALLBACK(on_changed), r);
		return dd;
	}
	case wb::SettingType::String: {
		GtkWidget *e = gtk_entry_new();
		gtk_widget_set_size_request(e, 320, -1);
		gtk_widget_set_halign(e, GTK_ALIGN_START);
		std::string hint = !d.hint.empty() ? d.hint : d.def;
		if (!hint.empty())
			gtk_entry_set_placeholder_text(GTK_ENTRY(e), hint.c_str());
		r->entry = GTK_ENTRY(e);
		g_signal_connect_swapped(e, "changed", G_CALLBACK(on_changed), r);
		return e;
	}
	case wb::SettingType::Color: {
		r->color = make_color_control(d.hint, d.def, true);
		r->entry = r->color->entry;
		g_signal_connect_swapped(r->entry, "changed", G_CALLBACK(on_changed), r);
		gtk_widget_set_halign(r->color->box, GTK_ALIGN_START);
		return r->color->box;
	}
	case wb::SettingType::Font: {
		r->font = make_font_control("pango", d.def.empty() ? "Sans 10" : d.def);
		r->font->on_change = [r] { on_changed(r); };
		gtk_widget_set_halign(r->font->box, GTK_ALIGN_START);
		return r->font->box;
	}
	case wb::SettingType::Lines: {
		GtkWidget *frame = gtk_frame_new(nullptr);
		GtkWidget *tv = gtk_text_view_new();
		gtk_widget_set_size_request(tv, 320, 90);
		gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
		gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 4);
		gtk_frame_set_child(GTK_FRAME(frame), tv);
		gtk_widget_set_halign(frame, GTK_ALIGN_START);
		r->text = GTK_TEXT_VIEW(tv);
		g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), "changed",
				G_CALLBACK(on_changed), r);
		return frame;
	}
	case wb::SettingType::Custom:
		if (d.widget == "theme")
			return build_theme_block(ui);
		if (d.widget == "monitors")
			return build_monitors_block(ui);
		if (d.widget == "keybindings")
			return build_keybindings_block(ui);
		if (d.widget == "mousebindings")
			return build_mousebindings_block(ui);
		if (d.widget.rfind("program:", 0) == 0)
			return build_program_block(ui, d.widget.substr(8));
		if (d.widget == "programs-other")
			return build_other_programs_block(ui);
		if (d.widget.rfind("binding:", 0) == 0)
			return build_binding_block(ui, d.widget.substr(8));
		if (d.widget == "inputmethod")
			return build_input_method_block(ui);
		if (d.widget == "gtk-appearance")
			return build_gtk_block(ui);
		if (d.widget == "autostart-script")
			return build_script_block(ui);
		if (d.widget == "xdg-autostart")
			return build_xdg_block(ui);
		if (d.widget == "bell-test")
			return build_bell_test(ui);
		if (d.widget == "osd-test")
			return build_osd_test(ui);
		return wrapped_label(_("(This setting's editor isn't available.)"), true);
	}
	return nullptr;
}

/* A grouped setting after the group's first: its control, captioned, on
 * the group's line. */
void add_to_group(Ui *ui, const wb::SettingDef &d, const std::string &category,
		Ui::SettingRow *leader, GtkWidget *line) {
	auto r = std::make_unique<Ui::SettingRow>();
	r->def = &d;
	r->category = category;
	r->row = leader->row;
	r->bar = leader->bar;
	GtkWidget *item = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(item), wrapped_label(d.title, true));
	gtk_box_append(GTK_BOX(item), control_for(ui, r.get()));
	gtk_box_append(GTK_BOX(line), item);
	ui->rows.push_back(std::move(r));
}

GtkWidget *add_row(Ui *ui, const wb::SettingDef &d, const std::string &category,
		GtkWidget **group_line = nullptr) {
	auto r = std::make_unique<Ui::SettingRow>();
	r->def = &d;
	r->category = category;

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(row, "wb-setting");
	g_object_set_data(G_OBJECT(row), "wb-ui", ui);
	r->row = row;

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(bar, "wb-bar");
	r->bar = bar;
	gtk_box_append(GTK_BOX(row), bar);

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(body, TRUE);
	gtk_box_append(GTK_BOX(row), body);
	GtkWidget *control = control_for(ui, r.get());
	if (!d.group.empty() && group_line != nullptr) {
		/* The first of a group: the group's title and description, then a
		 * line of captioned controls (the others join it). */
		GtkWidget *title = gtk_label_new(nullptr);
		char *m = g_markup_printf_escaped("<b>%s</b>", d.group.c_str());
		gtk_label_set_markup(GTK_LABEL(title), m);
		g_free(m);
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_box_append(GTK_BOX(body), title);
		if (!d.description.empty())
			gtk_box_append(GTK_BOX(body), wrapped_label(d.description, true));
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
		gtk_widget_set_margin_top(line, 2);
		GtkWidget *item = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_box_append(GTK_BOX(item), wrapped_label(d.title, true));
		gtk_box_append(GTK_BOX(item), control);
		gtk_box_append(GTK_BOX(line), item);
		gtk_box_append(GTK_BOX(body), line);
		*group_line = line;
	} else if (d.type == wb::SettingType::Bool) {
		gtk_box_append(GTK_BOX(body), control);
		if (!d.description.empty()) {
			GtkWidget *desc = wrapped_label(d.description, true);
			gtk_widget_set_margin_start(desc, 28);  /* under the label */
			gtk_box_append(GTK_BOX(body), desc);
		}
	} else {
		GtkWidget *title = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(title), title_markup(*r, false).c_str());
		r->title = GTK_LABEL(title);
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_label_set_wrap(GTK_LABEL(title), TRUE);
		/* A warning sign before the title when a problem is found. */
		GtkWidget *title_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		r->problem_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
		gtk_widget_add_css_class(r->problem_icon, "wb-problem-icon");
		gtk_widget_set_visible(r->problem_icon, FALSE);
		gtk_box_append(GTK_BOX(title_line), r->problem_icon);
		gtk_box_append(GTK_BOX(title_line), title);
		gtk_box_append(GTK_BOX(body), title_line);
		if (!d.description.empty())
			gtk_box_append(GTK_BOX(body), wrapped_label(d.description, true));
		/* The problem: what's wrong, why, and a fix where there's one. */
		r->problem_note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_add_css_class(r->problem_note, "wb-problem");
		gtk_widget_set_margin_top(r->problem_note, 2);
		GtkWidget *note_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
		gtk_widget_add_css_class(note_icon, "wb-problem-icon");
		gtk_widget_set_valign(note_icon, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(r->problem_note), note_icon);
		GtkWidget *text = gtk_label_new("");
		gtk_label_set_wrap(GTK_LABEL(text), TRUE);
		gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_xalign(GTK_LABEL(text), 0);
		gtk_label_set_selectable(GTK_LABEL(text), TRUE);  /* file names to copy */
		gtk_widget_set_hexpand(text, TRUE);
		r->problem_text = GTK_LABEL(text);
		gtk_box_append(GTK_BOX(r->problem_note), text);
		r->problem_fix = gtk_button_new_with_label("");
		gtk_widget_set_valign(r->problem_fix, GTK_ALIGN_CENTER);
		g_object_set_data(G_OBJECT(r->problem_fix), "wb-row", r.get());
		g_signal_connect(r->problem_fix, "clicked", G_CALLBACK(on_problem_fix), ui);
		gtk_box_append(GTK_BOX(r->problem_note), r->problem_fix);
		gtk_widget_set_visible(r->problem_note, FALSE);
		gtk_box_append(GTK_BOX(body), r->problem_note);
		gtk_widget_set_margin_top(control, 2);
		gtk_box_append(GTK_BOX(body), control);
	}

	if (d.type == wb::SettingType::Custom) {
		r->bar = nullptr;  /* the editor shows its own state */
	} else {
		gtk_box_append(GTK_BOX(row), row_menu(r.get()));
	}
	ui->rows.push_back(std::move(r));
	return row;
}

/* ---- Table of contents, scrolling ---- */

double y_in_list(Ui *ui, GtkWidget *w) {
	graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), out;
	if (!gtk_widget_compute_point(w, ui->list, &p, &out))
		return 0;
	return out.y;
}

/* The sidebar's rows as the tree is now: a category's sections shown
 * while it's open (and their headings are, when searching). */
void show_tree(Ui *ui) {
	for (size_t i = 0; i < ui->categories.size(); ++i) {
		Ui::CategoryView &c = ui->categories[i];
		bool open = c.open_by_user || c.open_for_now;
		if (c.arrow != nullptr)
			gtk_image_set_from_icon_name(c.arrow, open ? "pan-down-symbolic" : "pan-end-symbolic");
	}
	for (const Ui::TocEntry &e : ui->toc_entries) {
		const Ui::CategoryView &c = ui->categories[e.category];
		bool there = gtk_widget_get_visible(c.heading) &&
				(!e.section || gtk_widget_get_visible(e.heading));
		bool open = c.open_by_user || c.open_for_now;
		gtk_widget_set_visible(e.row, there && (!e.section || open));
	}
}

std::string toc_text(GtkWidget *row);

/* An entry's key, kept across runs: "CATEGORY-ID/Section title" (or
 * "CATEGORY-ID" for the category itself). */
std::string entry_key(Ui *ui, const Ui::TocEntry &e) {
	std::string k = ui->categories[e.category].id;
	return e.section ? k + "/" + toc_text(e.row) : k;
}

/* What the sidebar shows, remembered as it changes, if asked to (the
 * sidebar's ⋯ menu): the page read, the categories opened. */
void remember_page(Ui *ui, const Ui::TocEntry &e) {
	if (view_get("reopen-last") == "1")
		view_set("last-page", entry_key(ui, e));
}

void remember_tree(Ui *ui) {
	if (view_get("remember-tree") != "1")
		return;
	std::string open;
	for (const Ui::CategoryView &c : ui->categories)
		if (c.open_by_user)
			open += (open.empty() ? "" : ",") + c.id;
	view_set("tree-open", open);
}

const Ui::TocEntry *entry_of(Ui *ui, GtkListBoxRow *row) {
	for (const Ui::TocEntry &e : ui->toc_entries)
		if (e.row == GTK_WIDGET(row))
			return &e;
	return nullptr;
}

void scroll_to(Ui *ui, GtkWidget *heading);

}  // namespace

void reveal_in_list(Ui *ui, GtkWidget *w) {
	/* A search may hide it: cleared first, the page laid out again, then
	 * scrolled to (scroll_to tries again once sizes settle). */
	if (ui->search != nullptr && *gtk_editable_get_text(GTK_EDITABLE(ui->search)) != '\0')
		gtk_editable_set_text(GTK_EDITABLE(ui->search), "");
	scroll_to(ui, w);
	gtk_widget_grab_focus(w);
}

namespace {

void scroll_to(Ui *ui, GtkWidget *heading) {
	GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(ui->scroller);
	ui->scrolling_to = true;
	gtk_adjustment_set_value(adj, y_in_list(ui, heading) - 8);
	ui->scrolling_to = false;
	bool first = ui->scroll_target == nullptr;
	ui->scroll_target = heading;
	ui->scroll_set = gtk_adjustment_get_value(adj);
	if (!first)
		return;  /* one already waiting: it takes this one */
	g_timeout_add_once(250, [](gpointer data) {
		auto *ui = static_cast<Ui *>(data);
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(ui->scroller);
		GtkWidget *target = ui->scroll_target;
		ui->scroll_target = nullptr;
		if (target == nullptr || std::abs(gtk_adjustment_get_value(adj) - ui->scroll_set) > 1)
			return;  /* the user scrolled since */
		ui->scrolling_to = true;
		gtk_adjustment_set_value(adj, y_in_list(ui, target) - 8);
		ui->scrolling_to = false;
	}, ui);
}

void on_toc_activated(GtkListBox *, GtkListBoxRow *row, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	const Ui::TocEntry *e = entry_of(ui, row);
	if (e == nullptr)
		return;
	Ui::CategoryView &c = ui->categories[e->category];
	if (!e->section) {
		/* A category: there, and open; clicked again where it is, closed. */
		bool open = c.open_by_user || c.open_for_now;
		bool here = gtk_list_box_get_selected_row(ui->toc) == row;
		c.open_by_user = !(open && here);
		c.open_for_now = false;
		remember_tree(ui);
	} else {
		/* A section: its category open (for now, as being read). */
		for (Ui::CategoryView &o : ui->categories)
			o.open_for_now = &o == &c;
	}
	scroll_to(ui, e->heading);
	gtk_list_box_select_row(ui->toc, row);
	show_tree(ui);
	remember_page(ui, *e);
}

/* While scrolling, the sidebar follows: the last heading (section or
 * category) at or above the top of the view; its category opens for now,
 * and one opened only for now closes when left. */
void on_scrolled(GtkAdjustment *adj, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (ui->scrolling_to)
		return;
	double top = gtk_adjustment_get_value(adj) + 24;
	const Ui::TocEntry *current = nullptr;
	for (const Ui::TocEntry &e : ui->toc_entries)
		if (gtk_widget_get_visible(e.heading) && y_in_list(ui, e.heading) <= top)
			current = &e;
	if (current == nullptr && !ui->toc_entries.empty())
		current = &ui->toc_entries.front();
	if (current == nullptr)
		return;
	bool searching = ui->search != nullptr &&
			*gtk_editable_get_text(GTK_EDITABLE(ui->search)) != '\0';
	if (!searching)
		for (size_t i = 0; i < ui->categories.size(); ++i)
			ui->categories[i].open_for_now = static_cast<int>(i) == current->category;
	show_tree(ui);
	if (GTK_LIST_BOX_ROW(current->row) != gtk_list_box_get_selected_row(ui->toc)) {
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(current->row));
		if (!searching)
			remember_page(ui, *current);
	}
}

/* The sidebar's toolbar: open or close every category; and its ⋯ menu,
 * what's kept across runs. */
void set_all_open(Ui *ui, bool open) {
	for (Ui::CategoryView &c : ui->categories) {
		c.open_by_user = open;
		if (!open)
			c.open_for_now = false;
	}
	show_tree(ui);
	remember_tree(ui);
}

GtkWidget *view_check(Ui *ui, const char *label, const char *key, const char *tip) {
	GtkWidget *b = gtk_check_button_new_with_label(label);
	gtk_widget_set_tooltip_text(b, tip);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(b), view_get(key) == "1");
	g_object_set_data_full(G_OBJECT(b), "wb-key", g_strdup(key), g_free);
	g_signal_connect(b, "toggled", G_CALLBACK(+[](GtkCheckButton *b, gpointer d) {
		auto *ui = static_cast<Ui *>(d);
		std::string key = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-key"));
		bool on = gtk_check_button_get_active(b);
		if (key == "pages-open") {
			set_lua_pages_open(ui, on);
			return;
		}
		view_set(key, on ? "1" : "0");
		if (key == "remember-tree")
			remember_tree(ui);
		if (key == "reopen-last" && on)
			if (GtkListBoxRow *row = gtk_list_box_get_selected_row(ui->toc))
				if (const Ui::TocEntry *e = entry_of(ui, row))
					remember_page(ui, *e);
	}), ui);
	return b;
}

GtkWidget *toc_toolbar(Ui *ui) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bar, "wb-toc-toolbar");
	gtk_widget_set_halign(bar, GTK_ALIGN_END);
	gtk_widget_set_margin_top(bar, 3);
	gtk_widget_set_margin_end(bar, 6);
	auto icon_button = [&bar](const char *icon, const char *tip) {
		GtkWidget *b = gtk_button_new_from_icon_name(icon);
		gtk_widget_add_css_class(b, "flat");
		gtk_widget_set_tooltip_text(b, tip);
		gtk_box_append(GTK_BOX(bar), b);
		return b;
	};
	g_signal_connect(icon_button("wbconf-expand-all-symbolic", _("Expand All")), "clicked",
			G_CALLBACK(+[](GtkButton *, gpointer d) { set_all_open(static_cast<Ui *>(d), true); }),
			ui);
	g_signal_connect(icon_button("wbconf-collapse-all-symbolic", _("Collapse All")), "clicked",
			G_CALLBACK(+[](GtkButton *, gpointer d) { set_all_open(static_cast<Ui *>(d), false); }),
			ui);
	GtkWidget *more = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-horizontal-symbolic");
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(more), FALSE);
	gtk_widget_set_tooltip_text(more, _("View Options"));
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_append(GTK_BOX(box), view_check(ui, _("Open at the Last Page"), "reopen-last",
			_("Start where you left off, rather than at the top")));
	gtk_box_append(GTK_BOX(box), view_check(ui, _("Remember Expanded Sections"),
			"remember-tree", _("Keep the sidebar's categories opened or closed as you left them")));
	gtk_box_append(GTK_BOX(box), view_check(ui, _("Keep Program Settings Open"), "pages-open",
			_("Show programs' settings pages unfolded")));
	gtk_popover_set_child(GTK_POPOVER(pop), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(more), pop);
	gtk_box_append(GTK_BOX(bar), more);
	return bar;
}

/* At start: the categories opened as left, and the page left on. */
void restore_view(Ui *ui) {
	if (view_get("remember-tree") == "1") {
		std::string open = "," + view_get("tree-open") + ",";
		for (Ui::CategoryView &c : ui->categories)
			c.open_by_user = open.find("," + c.id + ",") != std::string::npos;
		show_tree(ui);
	}
	if (view_get("reopen-last") != "1")
		return;
	std::string want = view_get("last-page");
	for (const Ui::TocEntry &e : ui->toc_entries)
		if (!want.empty() && entry_key(ui, e) == want && gtk_widget_get_visible(e.heading)) {
			for (Ui::CategoryView &o : ui->categories)
				o.open_for_now = &o == &ui->categories[e.category];
			scroll_to(ui, e.heading);
			gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(e.row));
			show_tree(ui);
			break;
		}
}

/* ---- Search ---- */

void on_search(GtkSearchEntry *entry, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string q = gtk_editable_get_text(GTK_EDITABLE(entry));
	/* The custom editors count as modified when they hold anything. */
	wb::WayboxSettings now = collect(ui);
	auto custom_set = [&now, ui](const std::string &widget) {
		if (widget == "theme")
			return now.theme_name.has_value();
		if (widget == "monitors")
			return !now.outputs.empty() || now.hdr_outputs.has_value();
		if (widget.rfind("program:", 0) == 0) {
			for (auto &p : ui->programs)
				if (p->in != nullptr && p->in->id == widget.substr(8))
					return p->listed || gtk_switch_get_active(p->on);
			return false;
		}
		if (widget.rfind("binding:", 0) == 0)
			return binding_set(ui, widget.substr(8));
		if (widget == "inputmethod")
			return now.input_method.has_value();
		if (widget == "programs-other") {
			for (auto &p : ui->programs)
				if (p->in == nullptr && !p->removed)
					return true;
			return false;
		}
		return false;
	};
	bool any = false;
	for (Ui::CategoryView &c : ui->categories) {
		bool cat_any = false;
		for (auto &[heading, rows] : c.sections) {
			bool sec_any = false;
			for (GtkWidget *w : rows) {
				bool show = false;
				for (auto &row : ui->rows) {
					if (row->row != w)
						continue;
					Ui::SettingRow *r = row.get();
					bool modified = r->def->type == wb::SettingType::Custom
							? custom_set(r->def->widget) : r->value.has_value();
					std::string cat = r->category + " " + r->def->group;
					bool hit = wb::setting_matches(*r->def, cat, q, modified,
							r->problem.has_value());
					/* The Shortcuts lists: filtered row by row. */
					if (r->def->widget == "keybindings" || r->def->widget == "mousebindings") {
						bool whole = q.empty() ||
								wb::setting_matches(*r->def, cat, q, false, false);
						hit = shortcuts_filter(ui, r->def->widget, q, whole);
					}
					show = show || hit;
				}
				gtk_widget_set_visible(w, show);
				sec_any = sec_any || show;
			}
			if (heading != nullptr)
				gtk_widget_set_visible(heading, sec_any);
			cat_any = cat_any || sec_any;
		}
		gtk_widget_set_visible(c.heading, cat_any);
		if (c.description != nullptr)
			gtk_widget_set_visible(c.description, cat_any && q.empty());
		/* Searching: every category with a match open, to show where. */
		c.open_for_now = !q.empty() && cat_any;
		any = any || cat_any;
	}
	show_tree(ui);
	gtk_widget_set_visible(GTK_WIDGET(ui->no_results), !any);
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(ui->scroller), 0);
}

}  // namespace

GtkWidget *build_settings_view(Ui *ui) {
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_string(css, kCss);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	/* The schema: the installed one, then the user's additions. */
	const char *xch = getenv("XDG_CONFIG_HOME");
	std::string user_dir = (xch && *xch ? std::string(xch) : home_dir() + "/.config") +
			"/waybox/wbconf";
	std::vector<std::string> errors;
	const char *override_dir = getenv("WBCONF_SCHEMA_DIR");  /* tests, development */
	ui->schema = wb::load_settings_schema(
			{override_dir && *override_dir ? override_dir : WB_SCHEMA_DIR, user_dir}, &errors);
	for (const std::string &e : errors)
		g_warning("%s", e.c_str());

	/* Known programs join the list as settings of their own, from their
	 * integration files: at the start of Integrations. */
	{
		std::string problem;
		std::optional<wb::RcDocument> doc = load_document(&problem);
		load_integrations_data(ui, doc ? doc->read() : wb::WayboxSettings{});
		wb::SettingCategory *cat = nullptr;
		for (wb::SettingCategory &c : ui->schema)
			if (c.id == "integrations")
				cat = &c;
		if (cat == nullptr) {
			ui->schema.push_back({"integrations", _("Integrations"), "", {}});
			cat = &ui->schema.back();
		}
		std::vector<wb::SettingDef> programs = program_settings(ui);
		cat->settings.insert(cat->settings.begin(), programs.begin(), programs.end());
	}
	/* GTK programs' themes: at the end of Appearance. */
	for (wb::SettingCategory &c : ui->schema)
		if (c.id == "appearance")
			c.settings.push_back(gtk_appearance_setting());

	GtkWidget *paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	GtkWidget *toc_scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(toc_scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(toc_scroller, 170, -1);
	GtkWidget *toc = gtk_list_box_new();
	gtk_widget_add_css_class(toc, "navigation-sidebar");
	gtk_widget_add_css_class(toc, "wb-toc");
	ui->toc = GTK_LIST_BOX(toc);
	g_signal_connect(toc, "row-activated", G_CALLBACK(on_toc_activated), ui);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(toc_scroller), toc);
	gtk_widget_set_vexpand(toc_scroller, TRUE);
	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(side, "view");  /* one background with the list */
	gtk_box_append(GTK_BOX(side), toc_toolbar(ui));
	gtk_box_append(GTK_BOX(side), toc_scroller);
	gtk_box_append(GTK_BOX(paned), side);
	gtk_box_append(GTK_BOX(paned), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(scroller, TRUE);
	gtk_widget_set_vexpand(scroller, TRUE);
	ui->scroller = GTK_SCROLLED_WINDOW(scroller);
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_margin_start(list, 12);
	gtk_widget_set_margin_end(list, 12);
	gtk_widget_set_margin_bottom(list, 16);
	ui->list = list;
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
	gtk_box_append(GTK_BOX(paned), scroller);

	for (const wb::SettingCategory &cat : ui->schema) {
		Ui::CategoryView view;
		view.id = cat.id;
		view.heading = gtk_label_new(cat.title.c_str());
		gtk_label_set_xalign(GTK_LABEL(view.heading), 0);
		gtk_widget_add_css_class(view.heading, "wb-category");
		gtk_box_append(GTK_BOX(list), view.heading);
		if (!cat.description.empty()) {
			GtkWidget *desc = wrapped_label(cat.description, true);
			gtk_widget_set_margin_bottom(desc, 4);
			gtk_box_append(GTK_BOX(list), desc);
			view.description = desc;
		}

		/* The sidebar: the category (with its arrow), its sections after. */
		GtkWidget *toc_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		GtkWidget *arrow = gtk_image_new_from_icon_name("pan-end-symbolic");
		gtk_widget_add_css_class(arrow, "wb-toc-arrow");
		gtk_box_append(GTK_BOX(toc_line), arrow);
		GtkWidget *toc_label = gtk_label_new(cat.title.c_str());
		gtk_label_set_xalign(GTK_LABEL(toc_label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(toc_label), PANGO_ELLIPSIZE_END);
		gtk_widget_set_hexpand(toc_label, TRUE);
		gtk_box_append(GTK_BOX(toc_line), toc_label);
		view.problem_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
		gtk_widget_add_css_class(view.problem_icon, "wb-problem-icon");
		gtk_widget_set_visible(view.problem_icon, FALSE);
		gtk_box_append(GTK_BOX(toc_line), view.problem_icon);
		gtk_list_box_append(ui->toc, toc_line);
		view.toc = gtk_widget_get_parent(toc_line);
		view.arrow = GTK_IMAGE(arrow);
		int cat_index = static_cast<int>(ui->categories.size());
		ui->toc_entries.push_back({view.toc, cat_index, view.heading, false,
				view.problem_icon});

		std::string section = "\x01";  /* no section yet */
		std::string group;
		Ui::SettingRow *group_leader = nullptr;
		GtkWidget *group_line = nullptr;
		for (const wb::SettingDef &d : cat.settings) {
			if (d.section != section) {
				section = d.section;
				GtkWidget *h = nullptr;
				if (!section.empty()) {
					h = gtk_label_new(section.c_str());
					gtk_label_set_xalign(GTK_LABEL(h), 0);
					gtk_widget_add_css_class(h, "wb-section");
					gtk_box_append(GTK_BOX(list), h);
					GtkWidget *sl = gtk_label_new(section.c_str());
					gtk_label_set_xalign(GTK_LABEL(sl), 0);
					gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
					gtk_widget_set_hexpand(sl, TRUE);
					GtkWidget *sline = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
					gtk_widget_add_css_class(sline, "wb-toc-section");
					gtk_box_append(GTK_BOX(sline), sl);
					GtkWidget *sicon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
					gtk_widget_add_css_class(sicon, "wb-problem-icon");
					gtk_widget_set_visible(sicon, FALSE);
					gtk_box_append(GTK_BOX(sline), sicon);
					gtk_list_box_append(ui->toc, sline);
					ui->toc_entries.push_back({gtk_widget_get_parent(sline), cat_index, h, true,
							sicon});
				}
				view.sections.push_back({h, {}});
				group_leader = nullptr;  /* a group doesn't cross sections */
			}
			if (!d.group.empty() && d.group == group && group_leader != nullptr) {
				add_to_group(ui, d, cat.title, group_leader, group_line);
				continue;
			}
			group = d.group;
			GtkWidget *row = add_row(ui, d, cat.title, &group_line);
			group_leader = d.group.empty() ? nullptr : ui->rows.back().get();
			gtk_box_append(GTK_BOX(list), row);
			view.sections.back().second.push_back(row);
		}
		ui->categories.push_back(std::move(view));
	}
	ui->no_results = GTK_LABEL(wrapped_label(_("No settings found."), true));
	gtk_widget_set_margin_top(GTK_WIDGET(ui->no_results), 24);
	gtk_widget_set_visible(GTK_WIDGET(ui->no_results), FALSE);
	gtk_box_append(GTK_BOX(list), GTK_WIDGET(ui->no_results));
	/* Room after the last category, so it can scroll to the top too. */
	GtkWidget *room = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(room, -1, 420);
	gtk_box_append(GTK_BOX(list), room);

	g_signal_connect(gtk_scrolled_window_get_vadjustment(ui->scroller), "value-changed",
			G_CALLBACK(on_scrolled), ui);
	if (ui->search != nullptr)
		g_signal_connect(ui->search, "search-changed", G_CALLBACK(on_search), ui);
	if (!ui->categories.empty()) {
		ui->categories.front().open_for_now = true;
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(ui->categories.front().toc));
	}
	show_tree(ui);
	/* Once laid out: as left last time, if asked to. */
	g_timeout_add_once(200, [](gpointer d) { restore_view(static_cast<Ui *>(d)); }, ui);
	return paned;
}

namespace {

std::string toc_text(GtkWidget *row) {
	GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
	if (GTK_IS_LABEL(child))
		return gtk_label_get_text(GTK_LABEL(child));
	for (GtkWidget *c = gtk_widget_get_first_child(child); c; c = gtk_widget_get_next_sibling(c))
		if (GTK_IS_LABEL(c))
			return gtk_label_get_text(GTK_LABEL(c));
	return {};
}

/* WBCONF_TEST_TOC="click:Wallpaper;dump;search:bell;dump;scroll:0.6;dump":
 * the sidebar driven and described (stderr), a step every 0.8 s, once the
 * window is laid out. */
struct TocScript {
	Ui *ui;
	std::vector<std::string> steps;
	size_t next = 0;
};

gboolean toc_step(gpointer data) {
	auto *s = static_cast<TocScript *>(data);
	if (s->next >= s->steps.size()) {
		delete s;
		return G_SOURCE_REMOVE;
	}
	Ui *ui = s->ui;
	std::string step = s->steps[s->next++];
	if (step == "dump") {
		std::string out = "toc-dump:";
		GtkListBoxRow *sel = gtk_list_box_get_selected_row(ui->toc);
		for (const Ui::TocEntry &e : ui->toc_entries) {
			if (!gtk_widget_get_visible(e.row))
				continue;
			const Ui::CategoryView &c = ui->categories[e.category];
			bool open = c.open_by_user || c.open_for_now;
			out += std::string("|") + (e.section ? "." : open ? "v" : ">") + toc_text(e.row);
			if (GTK_WIDGET(sel) == e.row)
				out += "*";
		}
		fprintf(stderr, "%s\n", out.c_str());
	} else if (step == "expand-all" || step == "collapse-all") {
		set_all_open(ui, step == "expand-all");
	} else if (step.rfind("click:", 0) == 0) {
		for (const Ui::TocEntry &e : ui->toc_entries)
			if (toc_text(e.row) == step.substr(6)) {
				g_signal_emit_by_name(ui->toc, "row-activated", e.row);
				break;
			}
	} else if (step.rfind("search:", 0) == 0) {
		gtk_editable_set_text(GTK_EDITABLE(ui->search), step.substr(7).c_str());
	} else if (step.rfind("scroll:", 0) == 0) {
		/* To that fraction of the way down. */
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(ui->scroller);
		double f = g_ascii_strtod(step.c_str() + 7, nullptr);
		gtk_adjustment_set_value(adj, f * (gtk_adjustment_get_upper(adj) -
				gtk_adjustment_get_page_size(adj)));
	} else if (step == "changes") {
		/* "changes-dump:N|tip:LINES,LONGEST|where=values|..." */
		std::vector<UnsavedChange> changes = unsaved_changes(ui);
		const char *tip = gtk_widget_get_tooltip_text(ui->changes_button);
		size_t lines = 0, longest = 0;
		if (tip != nullptr) {
			for (const char *line = tip; line != nullptr;) {
				const char *nl = strchr(line, '\n');
				std::string l(line, nl ? static_cast<size_t>(nl - line) : strlen(line));
				longest = std::max(longest, static_cast<size_t>(g_utf8_strlen(l.c_str(), -1)));
				++lines;
				line = nl ? nl + 1 : nullptr;
			}
		}
		std::string out = "changes-dump:" + std::to_string(changes.size()) + "|tip:" +
				std::to_string(lines) + "," + std::to_string(longest) + "|button:" +
				(gtk_widget_get_visible(ui->changes_button) ? "shown" : "hidden");
		for (const UnsavedChange &c : changes)
			out += "|" + c.item.where + "=" + wb::change_values(c.item, 30);
		fprintf(stderr, "%s\n", out.c_str());
	} else if (step == "problems") {
		/* "problems-dump:N|TITLE=TEXT=>FIX|...|toc:NAME,NAME" (FIX "-": no
		 * button; the sidebar entries showing a mark) */
		std::string out;
		int n = 0;
		for (auto &r : ui->rows) {
			if (!r->problem)
				continue;
			++n;
			bool fix = r->problem_fix != nullptr && gtk_widget_get_visible(r->problem_fix);
			out += "|" + r->def->title + "=" +
					(r->problem_text ? gtk_label_get_text(r->problem_text) : "") + "=>" +
					(fix ? gtk_button_get_label(GTK_BUTTON(r->problem_fix)) : "-") +
					(gtk_widget_get_visible(r->problem_note) ? "" : " (note hidden)");
		}
		std::string toc;
		for (const Ui::TocEntry &e : ui->toc_entries)
			if (e.problem_icon != nullptr && gtk_widget_get_visible(e.problem_icon))
				toc += (toc.empty() ? "" : ",") + toc_text(e.row);
		fprintf(stderr, "problems-dump:%d%s|toc:%s\n", n, out.c_str(), toc.c_str());
	} else if (step.rfind("fix:", 0) == 0) {
		/* Click the Nth problem's fix. */
		int want = atoi(step.c_str() + 4), n = 0;
		for (auto &r : ui->rows)
			if (r->problem && n++ == want && r->problem_fix != nullptr)
				g_signal_emit_by_name(r->problem_fix, "clicked");
	} else if (step.rfind("luapage:", 0) == 0) {
		/* "luapage-dump:ID|KEY=VALUE[!PROBLEM][(hidden)]...|error:..." */
		std::string id = step.substr(8);
		fprintf(stderr, "luapage-dump:%s%s\n", id.c_str(), lua_page_dump(ui, id).c_str());
	} else if (step == "shortcuts") {
		shortcut_dump(ui);
	} else if (step.rfind("shortcut-", 0) == 0) {
		shortcut_test_step(ui, step);
	} else if (step.rfind("progstatus:", 0) == 0) {
		/* "progstatus:ID" -> "progstatus:ID|on|STATUS[|warning]" */
		for (auto &p : ui->programs)
			if (p->in != nullptr && p->in->id == step.substr(11) && p->status != nullptr)
				fprintf(stderr, "progstatus:%s|%s|%s%s\n", p->in->id.c_str(),
						gtk_switch_get_active(p->on) ? "on" : "off",
						gtk_label_get_text(p->status),
						gtk_widget_has_css_class(GTK_WIDGET(p->status), "warning")
								? "|warning" : "");
	} else if (step.rfind("wallgrid:", 0) == 0) {
		/* "wallgrid:ID" -> "wallgrid:ID|a.png,b.png" */
		for (auto &p : ui->programs)
			if (p->in != nullptr && p->page != nullptr && p->in->page == "wallpaper" &&
					p->in->id == step.substr(9))
				fprintf(stderr, "wallgrid:%s|%s\n", p->in->id.c_str(),
						wallpaper_test_grid(p->page.get()).c_str());
	} else if (step.rfind("pagefold:", 0) == 0) {
		/* "pagefold:ID" -> "pagefold:ID|open|folded|hidden" */
		std::string id = step.substr(9);
		fprintf(stderr, "pagefold:%s|%s\n", id.c_str(), lua_page_fold(ui, id).c_str());
	} else if (step == "pages-open" || step == "pages-folded") {
		set_lua_pages_open(ui, step == "pages-open");
	} else if (step.rfind("luaset:", 0) == 0) {
		/* "luaset:ID.KEY=VALUE" (or ID.click=KEY), timed as the steps are. */
		std::string rest = step.substr(7);
		size_t dot = rest.find('.'), eq = rest.find('=');
		if (dot != std::string::npos && eq != std::string::npos && dot < eq)
			lua_page_test(ui, rest.substr(0, dot), rest.substr(dot + 1, eq - dot - 1),
					rest.substr(eq + 1));
	} else if (step == "save") {
		on_save(nullptr, ui);
	} else if (step.rfind("set:", 0) == 0) {
		/* "set:ID=VALUE": as WBCONF_TEST_SET, timed as the steps are. */
		apply_test_values(ui, step.c_str() + 4);
	} else if (step == "status") {
		fprintf(stderr, "status-dump:%s\n",
				ui->status != nullptr ? gtk_label_get_text(ui->status) : "");
	} else if (step == "sources") {
		fprintf(stderr, "sources-dump:%s\n", sources_dump(ui).c_str());
	} else if (step == "open-changes") {
		open_changes(ui);
	} else if (step == "popover-size") {
		/* "popover-size:W,H,WINDOW_W,WINDOW_H" */
		GtkWidget *win = gtk_widget_get_ancestor(ui->changes_button, GTK_TYPE_WINDOW);
		fprintf(stderr, "popover-size:%d,%d,%d,%d\n",
				gtk_widget_get_width(ui->changes_popover),
				gtk_widget_get_height(ui->changes_popover),
				win ? gtk_widget_get_width(win) : 0, win ? gtk_widget_get_height(win) : 0);
	} else if (step.rfind("goto-change:", 0) == 0) {
		/* Click the popover's Nth line; then "goto:OFFSET" (the target's
		 * distance from the top of the view: small when scrolled to). */
		size_t n = static_cast<size_t>(atoi(step.c_str() + 12));
		auto *list = static_cast<GtkListBox *>(
				g_object_get_data(G_OBJECT(ui->changes_popover), "wb-list"));
		std::vector<UnsavedChange> changes = unsaved_changes(ui);
		if (list != nullptr && n < changes.size()) {
			GtkWidget *target = changes[n].target;
			g_signal_emit_by_name(list, "row-activated", gtk_list_box_get_row_at_index(list,
					static_cast<int>(n)));
			GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(ui->scroller);
			if (target != nullptr)
				fprintf(stderr, "goto:%.0f|popover:%s\n",
						y_in_list(ui, target) - gtk_adjustment_get_value(adj),
						gtk_widget_get_visible(ui->changes_popover) ? "open" : "closed");
		}
	}
	return G_SOURCE_CONTINUE;
}

}  // namespace

void start_toc_script(Ui *ui, const char *spec) {
	auto *s = new TocScript{ui, {}};
	std::string all = spec;
	size_t start = 0;
	while (start <= all.size()) {
		size_t semi = all.find(';', start);
		std::string step = all.substr(start, semi == std::string::npos ? std::string::npos
				: semi - start);
		if (!step.empty())
			s->steps.push_back(step);
		if (semi == std::string::npos)
			break;
		start = semi + 1;
	}
	g_timeout_add(800, toc_step, s);
}

void focus_category(Ui *ui, const std::string &category_id) {
	for (const Ui::CategoryView &c : ui->categories) {
		if (c.id != category_id)
			continue;
		scroll_to(ui, c.heading);
		for (Ui::CategoryView &o : ui->categories)
			o.open_for_now = &o == &c;
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(c.toc));
		show_tree(ui);
		return;
	}
}

void populate_rows(Ui *ui, const wb::RcDocument *doc) {
	for (auto &r : ui->rows) {
		if (r->def->type == wb::SettingType::Custom)
			continue;
		r->saved = doc != nullptr ? doc->get(r->def->rc) : std::nullopt;
		r->value = r->saved;
		show_value(ui, r.get());
	}
}

void apply_test_values(Ui *ui, const char *spec) {
	/* "id=value;id=;..." (empty: reset): as if changed in the window. */
	std::string all = spec;
	size_t start = 0;
	while (start < all.size()) {
		size_t end = all.find(';', start);
		std::string item = all.substr(start, end == std::string::npos ? std::string::npos
				: end - start);
		size_t eq = item.find('=');
		/* "inputmethod=ID": choose that input method ("" none). */
		if (eq != std::string::npos && item.substr(0, eq) == "inputmethod")
			choose_input_method(ui, item.substr(eq + 1));
		if (eq != std::string::npos && item.rfind("gtk.", 0) == 0)
			gtk_test_value(ui, item.substr(0, eq), item.substr(eq + 1));
		/* "script.LINE=on|off", "xdg.FILE=on|off": startup sources. */
		if (eq != std::string::npos && (item.rfind("script.", 0) == 0 ||
				item.rfind("xdg.", 0) == 0))
			source_test_value(ui, item.substr(0, eq), item.substr(eq + 1));
		/* "binding.ID=on|off": a key-bound program's "Bind to a key". */
		if (eq != std::string::npos && item.rfind("binding.", 0) == 0)
			for (auto &b : ui->bindings)
				if (b->in != nullptr && b->in->id == item.substr(8, eq - 8))
					gtk_switch_set_active(b->on, item.substr(eq + 1) == "on");
		/* "program.ID=on|off": a known program's "start with the session". */
		if (eq != std::string::npos && item.rfind("program.", 0) == 0)
			for (auto &p : ui->programs)
				if (p->in != nullptr && p->in->id == item.substr(8, eq - 8))
					gtk_switch_set_active(p->on, item.substr(eq + 1) == "on");
		/* "option.ID.OPTION=VALUE": set a known program's option through its
		 * widget, as the user would; a colour's "rgb(...)" through the
		 * colour button. */
		if (eq != std::string::npos && item.rfind("option.", 0) == 0) {
			std::string key = item.substr(7, eq - 7);
			size_t dot = key.rfind('.');
			std::string v = item.substr(eq + 1);
			for (auto &p : ui->programs) {
				if (dot == std::string::npos || p->in == nullptr ||
						p->in->id != key.substr(0, dot))
					continue;
				auto it = p->options.find(key.substr(dot + 1));
				if (it == p->options.end())
					continue;
				OptionField &f = *it->second;
				GdkRGBA rgba;
				if (f.color != nullptr && v.rfind("rgb", 0) == 0 &&
						gdk_rgba_parse(&rgba, v.c_str())) {
					gtk_color_dialog_button_set_rgba(f.color, &rgba);
				} else {
					set_option_field_value(f, v);
					if (f.on_change)
						f.on_change();
				}
			}
		}
				/* "wallpaper.ID=PATH" (or random=on|off, every=N, mode=M): use that
		 * program's wallpaper page, as a click would. */
		if (eq != std::string::npos && item.rfind("wallpaper.", 0) == 0) {
			for (auto &p : ui->programs) {
				if (p->in == nullptr || p->page == nullptr || p->in->page != "wallpaper")
					continue;
				std::string prefix = "wallpaper." + p->in->id;
				if (item.rfind(prefix + "=", 0) == 0)
					wallpaper_test_choose(p->page.get(), item.substr(eq + 1));
				else if (item.rfind(prefix + ".", 0) == 0)  /* .random=on, .every=N */
					wallpaper_test_choose(p->page.get(), item.substr(prefix.size() + 1));
			}
		}
		/* "monitor-make.NAME=TEXT" (-model, -desc): as if the monitor said so. */
		for (const char *what : {"make", "model", "serial", "desc"}) {
			std::string prefix = std::string("monitor-") + what + ".";
			if (eq != std::string::npos && item.rfind(prefix, 0) == 0)
				arranger_test_describe(ui, what, item.substr(prefix.size(), eq - prefix.size()),
						item.substr(eq + 1));
		}
		/* "reload=1": read rc.xml again (after monitor-make... above, as
		 * if the monitors had said so from the start). */
		if (eq != std::string::npos && item.substr(0, eq) == "reload") {
			std::string problem;
			if (auto doc = load_document(&problem))
				populate(ui, doc->read(), &*doc);
		}
		/* "colorpick.ID=1": that color setting's eyedropper. */
		if (eq != std::string::npos && item.rfind("colorpick.", 0) == 0)
			for (auto &r : ui->rows)
				if (r->def->id == item.substr(10, eq - 10) && r->color != nullptr)
					g_signal_emit_by_name(r->color->pick, "clicked");
		/* "save=1": save now, as Ctrl+S (to change more and save again). */
		if (eq != std::string::npos && item.substr(0, eq) == "save")
			on_save(nullptr, ui);
		/* "monitor.NAME.KEY=VALUE": a monitor card's control. */
		if (eq != std::string::npos && item.rfind("monitor.", 0) == 0) {
			std::string path = item.substr(8, eq - 8);
			size_t dot = path.rfind('.');
			if (dot != std::string::npos)
				monitor_test_set(ui, path.substr(0, dot), path.substr(dot + 1),
						item.substr(eq + 1));
		}
		/* "luapage.ID.KEY=VALUE" sets a Lua page's control; "luapage.ID.click=KEY"
		 * clicks its button. */
		if (eq != std::string::npos && item.rfind("luapage.", 0) == 0) {
			std::string path = item.substr(8, eq - 8);
			size_t dot = path.find('.');
			if (dot != std::string::npos)
				lua_page_test(ui, path.substr(0, dot), path.substr(dot + 1), item.substr(eq + 1));
		}
		/* "osd-test=1": click the on-screen display's Test. */
		if (eq != std::string::npos && item.substr(0, eq) == "osd-test")
			show_osd_test(ui);
		/* "bell-test=1": click the bell's Test. */
		if (eq != std::string::npos && item.substr(0, eq) == "bell-test")
			ring_bell_test(ui);
		/* "output-manager-stop=1": the Monitors notice's Stop. */
		if (eq != std::string::npos && item.substr(0, eq) == "output-manager-stop")
			output_manager_test_stop(ui);
		/* "identify=1": click Identify. */
		if (eq != std::string::npos && item.substr(0, eq) == "identify" &&
				ui->arranger.identify != nullptr)
			g_signal_emit_by_name(ui->arranger.identify, "clicked");
		/* "arrange.NAME=X,Y": drop that monitor at X,Y in the arranger. */
		if (eq != std::string::npos && item.rfind("arrange.", 0) == 0)
			if (auto p = wb::parse_position(item.substr(eq + 1)))
				arranger_test_drop(ui, item.substr(8, eq - 8), p->first, p->second);
		/* "configure.ID=1": click that program's Configure… button. */
		if (eq != std::string::npos && item.rfind("configure.", 0) == 0)
			for (auto &p : ui->programs)
				if (p->in != nullptr && p->in->id == item.substr(10, eq - 10) &&
						p->configure != nullptr)
					g_signal_emit_by_name(p->configure, "clicked");
				/* "bind.ID=KEY": turn on that program's key binding. */
		if (eq != std::string::npos && item.rfind("bind.", 0) == 0)
			for (auto &b : ui->bindings)
				if (b->in->id == item.substr(5, eq - 5)) {
					gtk_switch_set_active(b->on, TRUE);
					gtk_editable_set_text(GTK_EDITABLE(b->key), item.substr(eq + 1).c_str());
				}
		if (eq != std::string::npos)
			for (auto &r : ui->rows)
				if (r->def->id == item.substr(0, eq)) {
					std::string v = item.substr(eq + 1);
					r->value = v.empty() ? std::nullopt : std::optional<std::string>(v);
					show_value(ui, r.get());
				}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
}

void rows_saved(Ui *ui) {
	for (auto &r : ui->rows) {
		r->saved = r->value;
		refresh_marker(r.get());
	}
}

bool rows_unsaved(Ui *ui) {
	for (auto &r : ui->rows)
		if (r->value != r->saved)
			return true;
	return false;
}

void reset_all_rows(Ui *ui) {
	for (auto &r : ui->rows) {
		if (r->def->type == wb::SettingType::Custom)
			continue;
		r->value.reset();
		show_value(ui, r.get());
	}
	update_status(ui);
}

std::optional<std::string> setting_if_set(Ui *ui, const std::string &id) {
	for (auto &r : ui->rows)
		if (r->def->id == id)
			return r->value;
	return std::nullopt;
}

std::string setting_value(Ui *ui, const std::string &id) {
	for (auto &r : ui->rows)
		if (r->def->id == id)
			return r->value.value_or(r->def->def);
	return {};
}

}  // namespace wbconf
