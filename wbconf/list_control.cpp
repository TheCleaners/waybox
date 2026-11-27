/*
 * A list control (a page's list { }): an ordered list of names, each with
 * Move Up, Move Down and Remove; Add… opens a popover to type a name or
 * pick one of the suggestions not in the list yet.
 */
#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

void rebuild(ListControl *c, int focus_row = -1, const char *focus_what = nullptr);

void changed(ListControl *c) {
	if (!c->syncing && c->on_change)
		c->on_change();
}

void on_row_button(GtkButton *b, gpointer data) {
	auto *c = static_cast<ListControl *>(data);
	size_t i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "wb-index"));
	const char *what = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-what"));
	bool done = false;
	int focus = -1;
	if (g_strcmp0(what, "up") == 0) {
		done = wb::names::move(c->items, i, -1);
		focus = static_cast<int>(i) - 1;
	} else if (g_strcmp0(what, "down") == 0) {
		done = wb::names::move(c->items, i, 1);
		focus = static_cast<int>(i) + 1;
	} else {
		done = wb::names::remove(c->items, i);
		focus = std::min(static_cast<int>(i), static_cast<int>(c->items.size()) - 1);
	}
	if (!done)
		return;
	/* Focus stays with what was pressed, where it went (keyboard use). */
	rebuild(c, focus, what);
	changed(c);
}

GtkWidget *row_button(ListControl *c, size_t i, const char *what, const char *icon,
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

/* The label a suggestion is shown with ("" : its name). */
std::string label_of(const ListControl *c, const std::string &name) {
	for (size_t i = 0; i < c->suggestions.size() && i < c->suggestion_labels.size(); ++i)
		if (c->suggestions[i] == name && !c->suggestion_labels[i].empty())
			return c->suggestion_labels[i];
	return "";
}

void rebuild(ListControl *c, int focus_row, const char *focus_what) {
	while (GtkWidget *row = gtk_widget_get_first_child(c->rows))
		gtk_list_box_remove(GTK_LIST_BOX(c->rows), row);
	GtkWidget *focus = nullptr;
	for (size_t i = 0; i < c->items.size(); ++i) {
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		gtk_widget_set_margin_start(line, 10);
		gtk_widget_set_margin_end(line, 4);
		gtk_widget_set_margin_top(line, 2);
		gtk_widget_set_margin_bottom(line, 2);
		GtkWidget *text = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_hexpand(text, TRUE);
		GtkWidget *name = gtk_label_new(c->items[i].c_str());
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(text), name);
		/* A known name's description, dimmed after it. */
		std::string label = label_of(c, c->items[i]);
		if (!label.empty()) {
			GtkWidget *d = gtk_label_new(label.c_str());
			gtk_widget_add_css_class(d, "dim-label");
			gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_END);
			gtk_box_append(GTK_BOX(text), d);
		}
		gtk_box_append(GTK_BOX(line), text);
		bool last = i + 1 == c->items.size();
		GtkWidget *up = row_button(c, i, "up", "go-up-symbolic", _("Move Up"), i > 0);
		GtkWidget *down = row_button(c, i, "down", "go-down-symbolic", _("Move Down"), !last);
		GtkWidget *rm = row_button(c, i, "remove", "list-remove-symbolic", _("Remove"), true);
		gtk_box_append(GTK_BOX(line), up);
		gtk_box_append(GTK_BOX(line), down);
		gtk_box_append(GTK_BOX(line), rm);
		gtk_list_box_append(GTK_LIST_BOX(c->rows), line);
		if (static_cast<int>(i) == focus_row && focus_what != nullptr) {
			GtkWidget *want = g_strcmp0(focus_what, "up") == 0 ? up
					: g_strcmp0(focus_what, "down") == 0 ? down : rm;
			/* At the top or bottom now: the other way. */
			focus = gtk_widget_get_sensitive(want) ? want
					: g_strcmp0(focus_what, "up") == 0 ? down : up;
		}
	}
	if (focus != nullptr)
		gtk_widget_grab_focus(focus);
	else if (focus_what != nullptr && c->items.empty())
		gtk_widget_grab_focus(c->add);
}

void add_name(ListControl *c, const std::string &name) {
	if (!wb::names::add(c->items, name))
		return;
	rebuild(c);
	changed(c);
}

void on_suggestion(GtkButton *b, gpointer data) {
	auto *c = static_cast<ListControl *>(data);
	const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-name"));
	gtk_popover_popdown(GTK_POPOVER(c->popover));
	add_name(c, name ? name : "");
}

void on_entry_activate(GtkEntry *e, gpointer data) {
	auto *c = static_cast<ListControl *>(data);
	std::string name = gtk_editable_get_text(GTK_EDITABLE(e));
	gtk_popover_popdown(GTK_POPOVER(c->popover));
	add_name(c, name);
}

/* Opened: the entry empty, the suggestions not in the list yet. */
void on_popover_show(GtkWidget *, gpointer data) {
	auto *c = static_cast<ListControl *>(data);
	gtk_editable_set_text(GTK_EDITABLE(c->entry), "");
	while (GtkWidget *w = gtk_widget_get_first_child(c->suggest_box))
		gtk_box_remove(GTK_BOX(c->suggest_box), w);
	wb::names::List left = wb::names::not_in(c->suggestions, c->items);
	for (const std::string &name : left) {
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
	}
	gtk_widget_set_visible(c->suggest_scroll, !left.empty());
	gtk_widget_grab_focus(GTK_WIDGET(c->entry));
}

}  // namespace

std::unique_ptr<ListControl> make_list_control(const std::vector<std::string> &suggestions,
		const std::vector<std::string> &labels, const std::string &hint) {
	auto c = std::make_unique<ListControl>();
	c->suggestions = suggestions;
	c->suggestion_labels = labels;
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

	/* Add…: a name typed, or a suggestion. */
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
	gtk_entry_set_placeholder_text(c->entry, hint.empty() ? _("A name, then Enter") : hint.c_str());
	gtk_editable_set_width_chars(GTK_EDITABLE(c->entry), 24);
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

void list_control_set(ListControl *c, const wb::names::List &items) {
	c->syncing = true;
	c->items = items;
	rebuild(c);
	c->syncing = false;
}

/* For tests: as if Add, Move or Remove were used ("add:NAME", "up:N",
 * "down:N", "remove:N", N from 0), or Add… opened ("open"). */
bool list_control_test(ListControl *c, const std::string &action) {
	if (action == "open") {
		gtk_menu_button_popup(GTK_MENU_BUTTON(c->add));
		return true;
	}
	size_t colon = action.find(':');
	if (colon == std::string::npos)
		return false;
	std::string what = action.substr(0, colon), arg = action.substr(colon + 1);
	if (what == "add") {
		add_name(c, arg);
		return true;
	}
	size_t i = static_cast<size_t>(atoi(arg.c_str()));
	bool done = what == "up" ? wb::names::move(c->items, i, -1)
			: what == "down" ? wb::names::move(c->items, i, 1)
			: what == "remove" ? wb::names::remove(c->items, i) : false;
	if (done) {
		rebuild(c);
		changed(c);
	}
	return done;
}

}  // namespace wbconf
