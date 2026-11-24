/*
 * The settings list, generated from the schema (data/wbconf/settings.json),
 * in the manner of VS Code's settings editor: a search box, a table of
 * contents, and one scrolling list of settings, each with its category and
 * title, description, control, a bar marking it as set in rc.xml (or not
 * saved yet) and a menu to reset it or copy it.
 */
#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

const char *kCss = R"(
.wb-setting { padding: 6px 12px 10px 0; }
.wb-bar { min-width: 3px; margin-right: 12px; border-radius: 2px; }
.wb-bar.wb-set { background: @theme_selected_bg_color; }
.wb-bar.wb-unsaved { background: #e5a50a; }
.wb-category { font-size: 1.5em; font-weight: bold; margin-top: 22px; margin-bottom: 2px; }
.wb-section { font-size: 1.1em; font-weight: bold; margin-top: 12px; }
.wb-toc row { padding: 4px 10px; }
)";

/* "Section › " or the category, dimmed, then the title in bold. */
std::string title_markup(const Ui::SettingRow &r) {
	const std::string &crumb = r.def->section.empty() ? r.category : r.def->section;
	char *m = g_markup_printf_escaped("<span alpha=\"65%%\">%s: </span><b>%s</b>",
			crumb.c_str(), r.def->title.c_str());
	std::string out = m;
	g_free(m);
	return out;
}

void refresh_marker(Ui::SettingRow *r) {
	if (r->bar == nullptr)
		return;
	gtk_widget_remove_css_class(r->bar, "wb-set");
	gtk_widget_remove_css_class(r->bar, "wb-unsaved");
	if (r->value != r->saved) {
		gtk_widget_add_css_class(r->bar, "wb-unsaved");
		gtk_widget_set_tooltip_text(r->bar, _("Changed, not saved yet"));
	} else if (r->value) {
		gtk_widget_add_css_class(r->bar, "wb-set");
		gtk_widget_set_tooltip_text(r->bar, _("Set in rc.xml"));
	} else {
		gtk_widget_set_tooltip_text(r->bar, nullptr);
	}
}

void update_status(Ui *ui) {
	int n = 0;
	for (auto &r : ui->rows)
		if (r->value != r->saved)
			++n;
	if (n > 0) {
		char *t = g_strdup_printf(ngettext("%d setting changed, not saved yet",
				"%d settings changed, not saved yet", n), n);
		set_status(ui, t);
		g_free(t);
	}
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
		gtk_editable_set_text(GTK_EDITABLE(r->entry), r->value.value_or("").c_str());
		break;
	case wb::SettingType::Font:
		font_set(&r->font, v.empty() ? std::optional<std::string>("Sans 10")
				: std::optional<std::string>(v));
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
	case wb::SettingType::String: {
		std::string t = gtk_editable_get_text(GTK_EDITABLE(r->entry));
		if (!t.empty())
			v = t;
		break;
	}
	case wb::SettingType::Font:
		v = font_value(&r->font);
		if (v && *v == (d.def.empty() ? "Sans 10" : d.def) && !r->saved)
			v.reset();
		preview_refresh(ui);
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
}

void on_changed(Ui::SettingRow *r) {
	read_value(static_cast<Ui *>(g_object_get_data(G_OBJECT(r->row), "wb-ui")), r);
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
	r->value.reset();
	show_value(ui, r);
	if (r->def->type == wb::SettingType::Font)
		preview_refresh(ui);
	update_status(ui);
}

void on_copy_id(Ui::SettingRow *r) {
	copy_text(r->row, r->def->id);
}

void on_copy_xml(Ui::SettingRow *r) {
	copy_text(r->row, as_xml(*r->def, r->value.value_or(r->def->def)));
}

GtkWidget *row_menu(Ui::SettingRow *r) {
	GtkWidget *button = gtk_menu_button_new();
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
		gtk_label_set_markup(GTK_LABEL(l), title_markup(*r).c_str());
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
	case wb::SettingType::Font: {
		GtkWidget *box = make_font_picker(&r->font, G_CALLBACK(on_changed), r);
		gtk_widget_set_halign(box, GTK_ALIGN_START);
		return box;
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
		if (d.widget == "integrations")
			return build_integrations_block(ui);
		return wrapped_label(_("(This setting's editor isn't available.)"), true);
	}
	return nullptr;
}

GtkWidget *add_row(Ui *ui, const wb::SettingDef &d, const std::string &category) {
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
	if (d.type == wb::SettingType::Bool) {
		gtk_box_append(GTK_BOX(body), control);
		if (!d.description.empty()) {
			GtkWidget *desc = wrapped_label(d.description, true);
			gtk_widget_set_margin_start(desc, 28);  /* under the label */
			gtk_box_append(GTK_BOX(body), desc);
		}
	} else {
		GtkWidget *title = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(title), title_markup(*r).c_str());
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_label_set_wrap(GTK_LABEL(title), TRUE);
		gtk_box_append(GTK_BOX(body), title);
		if (!d.description.empty())
			gtk_box_append(GTK_BOX(body), wrapped_label(d.description, true));
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

void on_toc_activated(GtkListBox *, GtkListBoxRow *row, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int i = gtk_list_box_row_get_index(row);
	if (i >= 0 && i < static_cast<int>(ui->categories.size()))
		focus_category(ui, ui->categories[i].id);
}

/* While scrolling, the table of contents follows: the last category whose
 * heading is at or above the top of the view. */
void on_scrolled(GtkAdjustment *adj, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (ui->scrolling_to)
		return;
	double top = gtk_adjustment_get_value(adj) + 24;
	GtkWidget *current = nullptr;
	for (const Ui::CategoryView &c : ui->categories)
		if (gtk_widget_get_visible(c.heading) && y_in_list(ui, c.heading) <= top)
			current = c.toc;
	if (current == nullptr && !ui->categories.empty())
		current = ui->categories.front().toc;
	if (current != nullptr && GTK_LIST_BOX_ROW(current) != gtk_list_box_get_selected_row(ui->toc))
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(current));
}

/* ---- Search ---- */

void on_search(GtkSearchEntry *entry, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string q = gtk_editable_get_text(GTK_EDITABLE(entry));
	/* The custom editors count as modified when they hold anything. */
	wb::WayboxSettings now = collect(ui);
	auto custom_set = [&now](const std::string &widget) {
		if (widget == "theme")
			return now.theme_name.has_value();
		if (widget == "monitors")
			return !now.outputs.empty() || now.hdr_outputs.has_value();
		if (widget == "integrations")
			return !now.startup.empty();
		return false;
	};
	bool any = false;
	for (Ui::CategoryView &c : ui->categories) {
		bool cat_any = false;
		for (auto &[heading, rows] : c.sections) {
			bool sec_any = false;
			for (GtkWidget *w : rows) {
				Ui::SettingRow *r = nullptr;
				for (auto &row : ui->rows)
					if (row->row == w)
						r = row.get();
				bool modified = r != nullptr && (r->def->type == wb::SettingType::Custom
						? custom_set(r->def->widget) : r->value.has_value());
				bool show = r != nullptr && wb::setting_matches(*r->def, r->category, q,
						modified);
				gtk_widget_set_visible(w, show);
				sec_any = sec_any || show;
			}
			if (heading != nullptr)
				gtk_widget_set_visible(heading, sec_any);
			cat_any = cat_any || sec_any;
		}
		gtk_widget_set_visible(c.heading, cat_any);
		gtk_widget_set_visible(c.toc, cat_any);
		any = any || cat_any;
	}
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

	GtkWidget *paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	GtkWidget *toc_scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(toc_scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(toc_scroller, 190, -1);
	GtkWidget *toc = gtk_list_box_new();
	gtk_widget_add_css_class(toc, "navigation-sidebar");
	gtk_widget_add_css_class(toc, "wb-toc");
	ui->toc = GTK_LIST_BOX(toc);
	g_signal_connect(toc, "row-activated", G_CALLBACK(on_toc_activated), ui);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(toc_scroller), toc);
	gtk_box_append(GTK_BOX(paned), toc_scroller);
	gtk_box_append(GTK_BOX(paned), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(scroller, TRUE);
	gtk_widget_set_vexpand(scroller, TRUE);
	ui->scroller = GTK_SCROLLED_WINDOW(scroller);
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_margin_start(list, 18);
	gtk_widget_set_margin_end(list, 18);
	gtk_widget_set_margin_bottom(list, 24);
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

		GtkWidget *toc_label = gtk_label_new(cat.title.c_str());
		gtk_label_set_xalign(GTK_LABEL(toc_label), 0);
		gtk_list_box_append(ui->toc, toc_label);
		view.toc = GTK_WIDGET(gtk_list_box_get_row_at_index(ui->toc,
				static_cast<int>(ui->categories.size())));

		std::string section = "\x01";  /* no section yet */
		for (const wb::SettingDef &d : cat.settings) {
			if (d.section != section) {
				section = d.section;
				GtkWidget *h = nullptr;
				if (!section.empty()) {
					h = gtk_label_new(section.c_str());
					gtk_label_set_xalign(GTK_LABEL(h), 0);
					gtk_widget_add_css_class(h, "wb-section");
					gtk_box_append(GTK_BOX(list), h);
				}
				view.sections.push_back({h, {}});
			}
			GtkWidget *row = add_row(ui, d, cat.title);
			gtk_box_append(GTK_BOX(list), row);
			view.sections.back().second.push_back(row);
		}
		ui->categories.push_back(std::move(view));
	}
	ui->no_results = GTK_LABEL(wrapped_label(_("No settings found."), true));
	gtk_widget_set_margin_top(GTK_WIDGET(ui->no_results), 24);
	gtk_widget_set_visible(GTK_WIDGET(ui->no_results), FALSE);
	gtk_box_append(GTK_BOX(list), GTK_WIDGET(ui->no_results));

	g_signal_connect(gtk_scrolled_window_get_vadjustment(ui->scroller), "value-changed",
			G_CALLBACK(on_scrolled), ui);
	if (ui->search != nullptr)
		g_signal_connect(ui->search, "search-changed", G_CALLBACK(on_search), ui);
	if (!ui->categories.empty())
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(ui->categories.front().toc));
	return paned;
}

void focus_category(Ui *ui, const std::string &category_id) {
	for (const Ui::CategoryView &c : ui->categories) {
		if (c.id != category_id)
			continue;
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(ui->scroller);
		ui->scrolling_to = true;
		gtk_adjustment_set_value(adj, y_in_list(ui, c.heading) - 8);
		ui->scrolling_to = false;
		gtk_list_box_select_row(ui->toc, GTK_LIST_BOX_ROW(c.toc));
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

std::string setting_value(Ui *ui, const std::string &id) {
	for (auto &r : ui->rows)
		if (r->def->id == id)
			return r->value.value_or(r->def->def);
	return {};
}

}  // namespace wbconf
