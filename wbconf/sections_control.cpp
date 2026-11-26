/*
 * A sections control (a page's sections { }): repeated sections, each a
 * row: its name and how many of its fields are set, Move Up, Move Down
 * and Remove; opened, the name ("Applies to") and the fields, empty ones
 * left out (the page's main settings apply). Add… as a list's: a name
 * typed, or one of the suggestions.
 */
#include "wbconf_ui.hpp"

namespace wbconf {

namespace page = wb::page;

namespace {

wb::OptionType field_type(page::Kind k) {
	switch (k) {
	case page::Kind::Number: return wb::OptionType::Number;
	case page::Kind::Switch: return wb::OptionType::Switch;
	case page::Kind::Choice: return wb::OptionType::Choice;
	case page::Kind::Color: return wb::OptionType::Color;
	case page::Kind::File: return wb::OptionType::File;
	case page::Kind::Folder: return wb::OptionType::Folder;
	case page::Kind::Image: return wb::OptionType::Image;
	default: return wb::OptionType::Text;
	}
}

void changed(SectionsControl *c) {
	if (!c->syncing && c->on_change)
		c->on_change();
}

/* "2 settings", "Nothing set: the settings above apply". */
std::string summary(const page::Section &s) {
	size_t n = s.values.size();
	if (n == 0)
		return _("nothing set");
	gchar *t = g_strdup_printf(ngettext("%zu setting", "%zu settings", n), n);
	std::string out = t;
	g_free(t);
	return out;
}

void rebuild(SectionsControl *c, int focus_row = -1, const char *focus_what = nullptr);

void on_row_button(GtkButton *b, gpointer data) {
	auto *c = static_cast<SectionsControl *>(data);
	size_t i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "wb-index"));
	const char *what = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-what"));
	if (i >= c->items.size())
		return;
	int focus = -1;
	if (g_strcmp0(what, "toggle") == 0) {
		c->expanded[i] = !c->expanded[i];
		rebuild(c, static_cast<int>(i), "toggle");
		return;
	}
	if (g_strcmp0(what, "up") == 0 || g_strcmp0(what, "down") == 0) {
		int by = g_strcmp0(what, "up") == 0 ? -1 : 1;
		long to = static_cast<long>(i) + by;
		if (to < 0 || to >= static_cast<long>(c->items.size()))
			return;
		std::swap(c->items[i], c->items[static_cast<size_t>(to)]);
		bool e = c->expanded[i];
		c->expanded[i] = c->expanded[static_cast<size_t>(to)];
		c->expanded[static_cast<size_t>(to)] = e;
		focus = static_cast<int>(to);
	} else {
		c->items.erase(c->items.begin() + static_cast<long>(i));
		c->expanded.erase(c->expanded.begin() + static_cast<long>(i));
		focus = std::min(static_cast<int>(i), static_cast<int>(c->items.size()) - 1);
	}
	rebuild(c, focus, what);
	changed(c);
}

GtkWidget *row_button(SectionsControl *c, size_t i, const char *what, const char *icon,
		const char *tip, bool sensitive) {
	GtkWidget *b = gtk_button_new_from_icon_name(icon);
	gtk_widget_add_css_class(b, "flat");
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_sensitive(b, sensitive);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(b), "wb-index", GUINT_TO_POINTER(i));
	g_object_set_data(G_OBJECT(b), "wb-what", const_cast<char *>(what));
	g_signal_connect(b, "clicked", G_CALLBACK(on_row_button), c);
	return b;
}

struct NameData {
	SectionsControl *c;
	size_t i;
	GtkLabel *title;
};

void on_name_changed(GtkEditable *e, gpointer data) {
	auto *d = static_cast<NameData *>(data);
	if (d->i >= d->c->items.size())
		return;
	d->c->items[d->i].name = gtk_editable_get_text(e);
	gtk_label_set_text(d->title, d->c->items[d->i].name.empty() ? _("(no name)")
			: d->c->items[d->i].name.c_str());
	changed(d->c);
}

void rebuild(SectionsControl *c, int focus_row, const char *focus_what) {
	while (GtkWidget *row = gtk_widget_get_first_child(c->rows))
		gtk_list_box_remove(GTK_LIST_BOX(c->rows), row);
	c->row_fields.clear();
	c->row_opts.clear();
	c->summaries.clear();
	c->name_entries.clear();
	c->expanded.resize(c->items.size(), false);
	GtkWidget *focus = nullptr;
	for (size_t i = 0; i < c->items.size(); ++i) {
		const page::Section &s = c->items[i];
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		gtk_widget_set_margin_start(row, 4);
		gtk_widget_set_margin_end(row, 4);
		gtk_widget_set_margin_top(row, 2);
		gtk_widget_set_margin_bottom(row, 2);
		/* Its line: open/close, name, what's set, the buttons. */
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		GtkWidget *toggle = row_button(c, i, "toggle", c->expanded[i] ? "pan-down-symbolic"
				: "pan-end-symbolic", c->expanded[i] ? _("Close") : _("Open"), true);
		gtk_box_append(GTK_BOX(line), toggle);
		GtkWidget *text = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_hexpand(text, TRUE);
		GtkWidget *title = gtk_label_new(s.name.empty() ? _("(no name)") : s.name.c_str());
		gtk_label_set_xalign(GTK_LABEL(title), 0);
		gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(text), title);
		GtkWidget *sum = gtk_label_new(summary(s).c_str());
		gtk_widget_add_css_class(sum, "dim-label");
		gtk_box_append(GTK_BOX(text), sum);
		c->summaries.push_back(GTK_LABEL(sum));
		gtk_box_append(GTK_BOX(line), text);
		bool last = i + 1 == c->items.size();
		GtkWidget *up = row_button(c, i, "up", "go-up-symbolic", _("Move Up"), i > 0);
		GtkWidget *down = row_button(c, i, "down", "go-down-symbolic", _("Move Down"), !last);
		GtkWidget *rm = row_button(c, i, "remove", "list-remove-symbolic", _("Remove"), true);
		gtk_box_append(GTK_BOX(line), up);
		gtk_box_append(GTK_BOX(line), down);
		gtk_box_append(GTK_BOX(line), rm);
		gtk_box_append(GTK_BOX(row), line);

		/* Opened: its name, then its fields. */
		GtkWidget *grid = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
		gtk_widget_set_margin_start(grid, 34);
		gtk_widget_set_margin_bottom(grid, 8);
		gtk_widget_set_visible(grid, c->expanded[i]);
		int r = 0;
		GtkWidget *nl = grid_label(grid, r, _("Applies to:"));
		(void) nl;
		GtkWidget *name = gtk_entry_new();
		gtk_editable_set_text(GTK_EDITABLE(name), s.name.c_str());
		gtk_editable_set_width_chars(GTK_EDITABLE(name), 28);
		auto *nd = new NameData{c, i, GTK_LABEL(title)};
		g_signal_connect_data(name, "changed", G_CALLBACK(on_name_changed), nd,
				[](gpointer d, GClosure *) { delete static_cast<NameData *>(d); },
				GConnectFlags(0));
		grid_control(grid, r++, name);
		c->name_entries.push_back(GTK_ENTRY(name));
		std::vector<std::unique_ptr<wb::IntegrationOption>> opts;
		std::vector<std::unique_ptr<OptionField>> fields;
		for (const page::Control &fc : c->fields) {
			auto o = std::make_unique<wb::IntegrationOption>();
			o->id = fc.key;
			o->label = fc.label.empty() ? fc.key : fc.label;
			o->type = field_type(fc.kind);
			o->choices = fc.choices;
			o->choice_labels = fc.choice_labels;
			o->no_default_choice = false;  /* "Default": not set here */
			o->min = fc.min;
			o->max = fc.max;
			o->unit = fc.unit;
			o->hint = fc.hint.empty() ? _("as above") : fc.hint;
			o->width_chars = fc.kind == page::Kind::Text ? 24 : 0;
			o->color_alpha = true;
			std::string key = fc.key;
			size_t at = i;
			auto f = make_option_field(*o, [c, at, key] {
				if (at >= c->items.size() || at >= c->row_fields.size())
					return;
				for (auto &field : c->row_fields[at])
					if (field->opt->id == key) {
						std::string v = option_field_value(*field);
						if (v.empty())
							c->items[at].values.erase(key);
						else
							c->items[at].values[key] = v;
					}
				gtk_label_set_text(c->summaries[at], summary(c->items[at]).c_str());
				changed(c);
			});
			auto it = s.values.find(fc.key);
			set_option_field_value(*f, it != s.values.end() ? it->second : "");
			grid_label(grid, r, (o->label + ":").c_str());
			grid_control(grid, r++, f->widget);
			opts.push_back(std::move(o));
			fields.push_back(std::move(f));
		}
		gtk_box_append(GTK_BOX(row), grid);
		c->row_opts.push_back(std::move(opts));
		c->row_fields.push_back(std::move(fields));
		gtk_list_box_append(GTK_LIST_BOX(c->rows), row);
		if (static_cast<int>(i) == focus_row && focus_what != nullptr) {
			GtkWidget *want = g_strcmp0(focus_what, "up") == 0 ? up
					: g_strcmp0(focus_what, "down") == 0 ? down
					: g_strcmp0(focus_what, "toggle") == 0 ? toggle
					: g_strcmp0(focus_what, "name") == 0 ? name : rm;
			focus = gtk_widget_get_sensitive(want) ? want
					: g_strcmp0(focus_what, "up") == 0 ? down : up;
		}
	}
	if (focus != nullptr)
		gtk_widget_grab_focus(focus);
	else if (focus_what != nullptr && c->items.empty())
		gtk_widget_grab_focus(c->add);
}

std::string label_of(const SectionsControl *c, const std::string &name) {
	for (size_t i = 0; i < c->suggestions.size() && i < c->suggestion_labels.size(); ++i)
		if (c->suggestions[i] == name && !c->suggestion_labels[i].empty())
			return c->suggestion_labels[i];
	return "";
}

/* A new section, opened, its name to finish ("app-name=" ends with '='). */
void add_section(SectionsControl *c, const std::string &name) {
	std::string n = name;
	while (!n.empty() && (n.back() == ' ' || n.back() == '\t'))
		n.pop_back();
	while (!n.empty() && (n.front() == ' ' || n.front() == '\t'))
		n.erase(0, 1);
	if (n.empty())
		return;
	c->items.push_back({n, {}});
	c->expanded.resize(c->items.size() - 1, false);
	c->expanded.push_back(true);
	rebuild(c, static_cast<int>(c->items.size()) - 1, "name");
	/* "app-name=": the cursor at the end, for the rest. */
	if (!c->name_entries.empty())
		gtk_editable_set_position(GTK_EDITABLE(c->name_entries.back()), -1);
	changed(c);
}

void on_suggestion(GtkButton *b, gpointer data) {
	auto *c = static_cast<SectionsControl *>(data);
	const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-name"));
	gtk_popover_popdown(GTK_POPOVER(c->popover));
	add_section(c, name ? name : "");
}

void on_entry_activate(GtkEntry *e, gpointer data) {
	auto *c = static_cast<SectionsControl *>(data);
	std::string name = gtk_editable_get_text(GTK_EDITABLE(e));
	gtk_popover_popdown(GTK_POPOVER(c->popover));
	add_section(c, name);
}

void on_popover_show(GtkWidget *, gpointer data) {
	auto *c = static_cast<SectionsControl *>(data);
	gtk_editable_set_text(GTK_EDITABLE(c->entry), "");
	while (GtkWidget *w = gtk_widget_get_first_child(c->suggest_box))
		gtk_box_remove(GTK_BOX(c->suggest_box), w);
	/* A suggestion ending with '=' is a start (app-name=): always offered;
	 * a whole one, while not there yet. */
	int shown = 0;
	for (const std::string &name : c->suggestions) {
		bool there = std::any_of(c->items.begin(), c->items.end(),
				[&name](const page::Section &s) { return s.name == name; });
		if (there && (name.empty() || name.back() != '='))
			continue;
		GtkWidget *b = gtk_button_new();
		gtk_widget_add_css_class(b, "flat");
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *n = gtk_label_new(name.c_str());
		gtk_label_set_xalign(GTK_LABEL(n), 0);
		gtk_box_append(GTK_BOX(line), n);
		std::string label = label_of(c, name);
		if (!label.empty()) {
			GtkWidget *d = gtk_label_new(label.c_str());
			gtk_widget_add_css_class(d, "dim-label");
			gtk_box_append(GTK_BOX(line), d);
		}
		gtk_button_set_child(GTK_BUTTON(b), line);
		g_object_set_data_full(G_OBJECT(b), "wb-name", g_strdup(name.c_str()), g_free);
		g_signal_connect(b, "clicked", G_CALLBACK(on_suggestion), c);
		gtk_box_append(GTK_BOX(c->suggest_box), b);
		++shown;
	}
	gtk_widget_set_visible(c->suggest_scroll, shown > 0);
	gtk_widget_grab_focus(GTK_WIDGET(c->entry));
}

}  // namespace

SectionsControl::~SectionsControl() = default;

std::unique_ptr<SectionsControl> make_sections_control(const page::Control &control) {
	auto c = std::make_unique<SectionsControl>();
	c->fields = control.children;
	c->suggestions = control.choices;
	c->suggestion_labels = control.choice_labels;
	c->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_hexpand(c->box, TRUE);
	c->rows = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(c->rows), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(c->rows, "frame");
	GtkWidget *none = gtk_label_new(_("None"));
	gtk_widget_add_css_class(none, "dim-label");
	gtk_widget_set_margin_top(none, 8);
	gtk_widget_set_margin_bottom(none, 8);
	gtk_list_box_set_placeholder(GTK_LIST_BOX(c->rows), none);
	gtk_box_append(GTK_BOX(c->box), c->rows);

	c->add = gtk_menu_button_new();
	GtkWidget *add_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(add_line), gtk_image_new_from_icon_name("list-add-symbolic"));
	gtk_box_append(GTK_BOX(add_line), gtk_label_new_with_mnemonic(_("_Add…")));
	gtk_menu_button_set_child(GTK_MENU_BUTTON(c->add), add_line);
	gtk_widget_set_halign(c->add, GTK_ALIGN_START);
	c->popover = gtk_popover_new();
	GtkWidget *pop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(pop, 4);
	gtk_widget_set_margin_end(pop, 4);
	gtk_widget_set_margin_top(pop, 4);
	gtk_widget_set_margin_bottom(pop, 4);
	c->entry = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(c->entry, control.hint.empty() ? _("Its name, then Enter")
			: control.hint.c_str());
	gtk_editable_set_width_chars(GTK_EDITABLE(c->entry), 26);
	g_signal_connect(c->entry, "activate", G_CALLBACK(on_entry_activate), c.get());
	gtk_box_append(GTK_BOX(pop), GTK_WIDGET(c->entry));
	c->suggest_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	c->suggest_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->suggest_scroll), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(c->suggest_scroll), 260);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(c->suggest_scroll),
			TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(c->suggest_scroll),
			TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(c->suggest_scroll), c->suggest_box);
	gtk_box_append(GTK_BOX(pop), c->suggest_scroll);
	gtk_popover_set_child(GTK_POPOVER(c->popover), pop);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(c->add), c->popover);
	g_signal_connect(c->popover, "show", G_CALLBACK(on_popover_show), c.get());
	gtk_box_append(GTK_BOX(c->box), c->add);
	return c;
}

void sections_control_set(SectionsControl *c, const std::vector<page::Section> &items) {
	c->syncing = true;
	c->items = items;
	c->expanded.assign(items.size(), false);
	rebuild(c);
	c->syncing = false;
}

/* Tests: "add:NAME", "up:N", "down:N", "remove:N", "open:N" (a row),
 * "rename:N:NAME", "set:N:FIELD=VALUE" (as typed or picked), "open" (Add…). */
bool sections_control_test(SectionsControl *c, const std::string &action) {
	if (action == "open") {
		gtk_menu_button_popup(GTK_MENU_BUTTON(c->add));
		return true;
	}
	size_t colon = action.find(':');
	if (colon == std::string::npos)
		return false;
	std::string what = action.substr(0, colon), arg = action.substr(colon + 1);
	if (what == "add") {
		add_section(c, arg);
		return true;
	}
	size_t i = static_cast<size_t>(atoi(arg.c_str()));
	if (i >= c->items.size())
		return false;
	std::string rest = arg.find(':') == std::string::npos ? "" : arg.substr(arg.find(':') + 1);
	if (what == "rename") {
		gtk_editable_set_text(GTK_EDITABLE(c->name_entries[i]), rest.c_str());
		return true;
	}
	if (what == "set") {
		size_t eq = rest.find('=');
		if (eq == std::string::npos)
			return false;
		for (auto &f : c->row_fields[i])
			if (f->opt->id == rest.substr(0, eq)) {
				set_option_field_value(*f, rest.substr(eq + 1));
				if (f->on_change)
					f->on_change();
				return true;
			}
		return false;
	}
	/* The row's own buttons. */
	const char *names[] = {"up", "down", "remove", "toggle"};
	std::string button = what == "open" ? "toggle" : what;
	for (const char *n : names)
		if (button == n) {
			GtkWidget *row = gtk_widget_get_first_child(c->rows);
			for (size_t k = 0; k < i && row != nullptr; ++k)
				row = gtk_widget_get_next_sibling(row);
			if (row == nullptr)
				return false;
			/* row > box > line > buttons */
			GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
			GtkWidget *line = gtk_widget_get_first_child(box);
			for (GtkWidget *w = gtk_widget_get_first_child(line); w != nullptr;
					w = gtk_widget_get_next_sibling(w))
				if (g_strcmp0(static_cast<const char *>(g_object_get_data(G_OBJECT(w),
						"wb-what")), n) == 0) {
					if (gtk_widget_get_sensitive(w))
						g_signal_emit_by_name(w, "clicked");
					return true;
				}
		}
	return false;
}

}  // namespace wbconf
