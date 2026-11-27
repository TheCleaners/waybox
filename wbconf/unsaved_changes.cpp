/*
 * The changes not saved yet, listed: a button beside the status line
 * ("3 changes not saved yet") whose tooltip names the first few and whose
 * popover lists them all, each "setting: before → after", a click taking
 * the page to it. Every line is kept to one short line (wb::one_line) and
 * the popover to a fixed size, scrolling: a long list or a value with line
 * breaks can't stretch it across the screen.
 */
#include "wbconf_ui.hpp"

namespace wbconf {

namespace {

const size_t kTooltipLines = 8;
const size_t kChars = 60;        /* a line's text, in characters */
const int kPopoverHeight = 360;  /* then it scrolls */
const int kPopoverWidth = 560;   /* at most; the labels are cut to fit */

const char *kSep = " \xE2\x80\xBA ";  /* " › " */

/* "Category › Section › Group › Title", without empty or repeated parts
 * ("Appearance › Theme", not "Appearance › Theme › Theme"; "Monitors ›
 * Margins › Top", not "Monitors › Top"). */
std::string where_of(const Ui::SettingRow &r) {
	std::string out, last;
	for (const std::string &part : {r.category, r.def->section, r.def->group, r.def->title}) {
		if (part.empty() || part == last)
			continue;
		if (!out.empty())
			out += " \xE2\x80\xBA ";
		out += part;
		last = part;
	}
	return out;
}

bool truthy(const std::string &v) {
	return v == "yes" || v == "true" || v == "on" || v == "1";
}

/* A setting's value as the page shows it: a choice's label, on/off,
 * minutes for seconds stored, "default" when unset. */
std::string shown_value(const wb::SettingDef &d, const std::optional<std::string> &v) {
	if (!v)  /* not set: the default, as the page shows it */
		return d.def.empty() ? std::string(_("default")) : shown_value(d, d.def);
	switch (d.type) {
	case wb::SettingType::Bool:
		return truthy(*v) ? _("on") : _("off");
	case wb::SettingType::Enum:
		for (const wb::SettingChoice &c : d.choices)
			if (c.value == *v)
				return c.label;
		return *v;
	case wb::SettingType::Int: {
		std::string s = *v;
		if (d.scale > 1) {
			char *end = nullptr;
			long n = strtol(v->c_str(), &end, 10);
			if (end != v->c_str() && *end == '\0')
				s = std::to_string(n / d.scale);
		}
		return d.unit.empty() ? s : s + " " + d.unit;
	}
	default:
		return v->empty() ? _("(empty)") : *v;
	}
}

std::string on_off(bool on) {
	return on ? _("on") : _("off");
}

void add(std::vector<UnsavedChange> &out, const std::string &where, GtkWidget *target,
		std::string before = "", std::string after = "") {
	out.push_back({{where, std::move(before), std::move(after)}, target});
}

/* <startup> entries by name. */
std::map<std::string, const wb::StartupSetting *> by_name(
		const std::vector<wb::StartupSetting> &v) {
	std::map<std::string, const wb::StartupSetting *> m;
	for (const wb::StartupSetting &s : v)
		m.emplace(s.name, &s);
	return m;
}

/* One program's <startup> change: started or not, or its command. */
void startup_change(const wb::StartupSetting *was, const wb::StartupSetting *now,
		const std::string &where, GtkWidget *target, std::vector<UnsavedChange> &out) {
	if (was == nullptr && now == nullptr)
		return;
	if (was == nullptr || now == nullptr) {
		add(out, where, target, on_off(was != nullptr), on_off(now != nullptr));
		return;
	}
	if (*was == *now)
		return;
	if (was->command != now->command)
		add(out, where, target, was->command, now->command);
	else
		add(out, where, target);  /* its options */
}

}  // namespace

std::vector<UnsavedChange> unsaved_changes(Ui *ui) {
	std::vector<UnsavedChange> out;
	if (!ui->loaded)
		return out;
	wb::WayboxSettings now = collect(ui);
	auto was_started = by_name(ui->shown.startup);
	auto now_started = by_name(now.startup);
	std::set<std::string> startup_done;
	bool theme = false, monitors = false, input = false, gtk = false, script = false,
			xdg = false;
	std::set<std::string> bindings_done;
	for (auto &rp : ui->rows) {
		const Ui::SettingRow &r = *rp;
		const wb::SettingDef &d = *r.def;
		std::string where = where_of(r);
		if (d.type != wb::SettingType::Custom) {
			if (r.value != r.saved)
				add(out, where, r.row, shown_value(d, r.saved), shown_value(d, r.value));
			continue;
		}
		const std::string &w = d.widget;
		if (w == "theme" && !theme) {
			theme = true;
			if (now.theme_name != ui->shown.theme_name)
				add(out, where, r.row, ui->shown.theme_name.value_or(_("default")),
						now.theme_name.value_or(_("default")));
		} else if (w == "monitors" && !monitors) {
			monitors = true;
			if (now.outputs != ui->shown.outputs || now.hdr_outputs != ui->shown.hdr_outputs)
				add(out, where, r.row);
		} else if (w == "inputmethod" && !input) {
			input = true;
			if (now.input_method != ui->shown.input_method)
				add(out, where, r.row,
						ui->shown.input_method ? ui->shown.input_method->command : _("none"),
						now.input_method ? now.input_method->command : _("none"));
		} else if (w == "gtk-appearance" && !gtk) {
			gtk = true;
			gtk_changes(ui, where, r.row, out);
		} else if (w == "autostart-script" && !script) {
			script = true;
			sources_changes(ui, true, where, r.row, out);
		} else if (w == "xdg-autostart" && !xdg) {
			xdg = true;
			sources_changes(ui, false, where, r.row, out);
		} else if (w == "apprules") {
			rule_changes(ui, where, r.row, out);
		} else if (w == "keybindings" || w == "mousebindings") {
			shortcut_changes(ui, w, where, r.row, out);
		} else if (w.rfind("binding:", 0) == 0) {
			bindings_done.insert(w.substr(8));
			binding_changes(ui, w.substr(8), where, r.row, out);
			lua_page_changes(ui, w.substr(8), where, r.row, out);
		} else if (w.rfind("program:", 0) == 0) {
			for (auto &p : ui->programs)
				if (p->in != nullptr && p->in->id == w.substr(8)) {
					startup_done.insert(p->name);
					auto a = was_started.find(p->name), b = now_started.find(p->name);
					startup_change(a == was_started.end() ? nullptr : a->second,
							b == now_started.end() ? nullptr : b->second, where, r.row, out);
					lua_page_changes(ui, p->in->id, where, r.row, out);
				}
		} else if (w == "programs-other") {
			/* Every other <startup> entry: custom programs, by name. */
			std::set<std::string> names;
			for (auto &[n, s] : was_started)
				names.insert(n);
			for (auto &[n, s] : now_started)
				names.insert(n);
			for (const std::string &n : names) {
				if (startup_done.count(n))
					continue;
				bool known = false;
				for (auto &p : ui->programs)
					known = known || (p->in != nullptr && p->name == n);
				if (known)
					continue;
				startup_done.insert(n);
				auto a = was_started.find(n), b = now_started.find(n);
				startup_change(a == was_started.end() ? nullptr : a->second,
						b == now_started.end() ? nullptr : b->second,
						where + " \xE2\x80\xBA " + n, r.row, out);
			}
		}
	}
	/* Whatever had no row shown (an editor not in the page): still listed,
	 * so the list always matches the count. */
	if (!theme && now.theme_name != ui->shown.theme_name)
		add(out, _("Theme"), nullptr, ui->shown.theme_name.value_or(""),
				now.theme_name.value_or(""));
	if (!monitors && (now.outputs != ui->shown.outputs ||
			now.hdr_outputs != ui->shown.hdr_outputs))
		add(out, _("Monitors"), nullptr);
	if (!input && now.input_method != ui->shown.input_method)
		add(out, _("Input method"), nullptr);
	if (!gtk)
		gtk_changes(ui, _("Applications (GTK)"), nullptr, out);
	if (!script)
		sources_changes(ui, true, _("Autostart script"), nullptr, out);
	if (!xdg)
		sources_changes(ui, false, _("Desktop autostart"), nullptr, out);
	for (auto &b : ui->bindings)
		if (b->in != nullptr && !bindings_done.count(b->in->id))
			binding_changes(ui, b->in->id, b->in->name, nullptr, out);
	std::set<std::string> names;
	for (auto &[n, s] : was_started)
		names.insert(n);
	for (auto &[n, s] : now_started)
		names.insert(n);
	for (const std::string &n : names)
		if (!startup_done.count(n)) {
			auto a = was_started.find(n), b = now_started.find(n);
			startup_change(a == was_started.end() ? nullptr : a->second,
					b == now_started.end() ? nullptr : b->second, n, nullptr, out);
		}
	return out;
}

namespace {

void on_change_activated(GtkListBox *, GtkListBoxRow *row, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	/* By position and name in a list made now: the widgets the popover was
	 * filled from may have gone with a reload since. */
	size_t i = static_cast<size_t>(gtk_list_box_row_get_index(row));
	auto *where = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "wb-where"));
	std::vector<UnsavedChange> changes = unsaved_changes(ui);
	GtkWidget *target = nullptr;
	if (i < changes.size() && where != nullptr && changes[i].item.where == where)
		target = changes[i].target;
	gtk_popover_popdown(GTK_POPOVER(ui->changes_popover));
	if (target != nullptr)
		reveal_in_list(ui, target);
}

GtkWidget *line_label(const std::string &text, bool dim, int max_chars = kChars) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), max_chars);
	if (dim)
		gtk_widget_add_css_class(l, "dim-label");
	return l;
}

/* "Appearance › Titlebar › Button order": the name, and where it is. */
std::pair<std::string, std::string> name_and_place(const std::string &where) {
	size_t at = where.rfind(kSep);
	if (at == std::string::npos)
		return {where, ""};
	return {where.substr(at + strlen(kSep)), where.substr(0, at)};
}

/* The popover's list, made each time it opens. */
void fill_popover(Ui *ui) {
	std::vector<UnsavedChange> changes = unsaved_changes(ui);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *heading = gtk_label_new(nullptr);
	char *t = g_strdup_printf(ngettext("%d change not saved yet", "%d changes not saved yet",
			static_cast<int>(changes.size())), static_cast<int>(changes.size()));
	gtk_label_set_text(GTK_LABEL(heading), t);
	g_free(t);
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_widget_add_css_class(heading, "heading");
	gtk_widget_set_margin_start(heading, 6);
	gtk_box_append(GTK_BOX(box), heading);

	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
	gtk_widget_add_css_class(list, "navigation-sidebar");
	for (const UnsavedChange &c : changes) {
		GtkWidget *row = gtk_list_box_row_new();
		GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		/* The setting's name first (cut last), then where it is, dimmed. */
		auto [name, place] = name_and_place(c.item.where);
		GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *name_label = line_label(wb::one_line(name, kChars / 2), false, kChars / 2);
		gtk_widget_add_css_class(name_label, "heading");
		gtk_box_append(GTK_BOX(top), name_label);
		if (!place.empty()) {
			GtkWidget *p = line_label(wb::one_line(place, kChars / 2), true, kChars / 2);
			gtk_widget_add_css_class(p, "caption");
			gtk_widget_set_hexpand(p, TRUE);
			gtk_widget_set_halign(p, GTK_ALIGN_END);
			gtk_label_set_xalign(GTK_LABEL(p), 1);
			gtk_box_append(GTK_BOX(top), p);
		}
		gtk_box_append(GTK_BOX(v), top);
		std::string values = wb::change_values(c.item, kChars / 2);
		gtk_box_append(GTK_BOX(v), line_label(values.empty() ? _("changed") : values, true));
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), v);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), c.target != nullptr);
		g_object_set_data_full(G_OBJECT(row), "wb-where", g_strdup(c.item.where.c_str()), g_free);
		if (c.target != nullptr)
			gtk_widget_set_tooltip_text(row, _("Go to this setting"));
		gtk_list_box_append(GTK_LIST_BOX(list), row);
	}
	g_signal_connect(list, "row-activated", G_CALLBACK(on_change_activated), ui);

	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(scroll), kPopoverWidth);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), kPopoverHeight);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_box_append(GTK_BOX(box), scroll);
	gtk_popover_set_child(GTK_POPOVER(ui->changes_popover), box);
	g_object_set_data(G_OBJECT(ui->changes_popover), "wb-list", list);
}

void on_popover_shown(GtkWidget *, gpointer data) {
	fill_popover(static_cast<Ui *>(data));
}

}  // namespace

GtkWidget *build_changes_button(Ui *ui) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-list-symbolic");
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(button, FALSE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
			_("Changes not saved yet"), -1);
	/* Up: the button sits at the window's bottom. (A menu button places
	 * its popover by its direction, not the popover's position.) */
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(button), GTK_ARROW_UP);
	GtkWidget *popover = gtk_popover_new();
	g_signal_connect(popover, "show", G_CALLBACK(on_popover_shown), ui);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	ui->changes_button = button;
	ui->changes_popover = popover;
	return button;
}

void refresh_changes_button(Ui *ui, const std::vector<UnsavedChange> &changes) {
	if (ui->changes_button == nullptr)
		return;
	gtk_widget_set_visible(ui->changes_button, !changes.empty());
	std::vector<wb::ChangeItem> items;
	for (const UnsavedChange &c : changes)
		items.push_back(c.item);
	std::string tip = changes.empty() ? "" : wb::change_tooltip(items, kTooltipLines, kChars,
			_("and %zu more: click to see all"));
	gtk_widget_set_tooltip_text(ui->changes_button, tip.empty() ? nullptr : tip.c_str());
	if (ui->status != nullptr)
		gtk_widget_set_tooltip_text(GTK_WIDGET(ui->status), tip.empty() ? nullptr : tip.c_str());
	if (changes.empty() && gtk_widget_get_visible(ui->changes_popover))
		gtk_popover_popdown(GTK_POPOVER(ui->changes_popover));
}

void open_changes(Ui *ui) {
	if (ui->changes_button != nullptr && gtk_widget_get_visible(ui->changes_button))
		gtk_menu_button_popup(GTK_MENU_BUTTON(ui->changes_button));
}

}  // namespace wbconf
