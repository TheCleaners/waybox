/*
 * The Shortcuts page (#236): rc.xml's key and mouse bindings, as key caps
 * and a line saying what each does, grouped as the action catalog groups
 * actions. Read-only for now: editing comes on top of this list.
 */
#include "wbconf_ui.hpp"

#include <algorithm>
#include <map>

#include "waybox/action_catalog.hpp"
#include "waybox/key_label.hpp"

namespace wbconf {

namespace {

/* The categories in the catalog's order, then "Other". */
std::vector<std::string> category_order() {
	std::vector<std::string> out;
	for (const wb::ActionInfo &a : wb::action_catalog())
		if (std::find(out.begin(), out.end(), a.category) == out.end())
			out.push_back(a.category);
	out.push_back("Other");
	return out;
}

std::string category_of(const std::vector<wb::BoundAction> &actions) {
	if (actions.empty() || actions[0].advanced)
		return "Other";
	const wb::ActionInfo *info = wb::action_info(actions[0].name);
	return info != nullptr ? info->category : "Other";
}

/* One key cap. */
GtkWidget *cap(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "wb-keycap");
	return l;
}

/* Caps for one key or button, side by side. */
GtkWidget *caps(const std::vector<std::string> &parts) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	for (const std::string &p : parts)
		gtk_box_append(GTK_BOX(box), cap(p));
	return box;
}

GtkWidget *dim(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "dim-label");
	return l;
}

/* A row: the keys (in a column as wide as the widest), what it does. */
GtkWidget *row(GtkSizeGroup *keys_column, GtkWidget *keys, const std::string &does,
		bool advanced) {
	GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_widget_add_css_class(r, "wb-shortcut-row");
	gtk_widget_set_halign(keys, GTK_ALIGN_START);
	GtkWidget *keys_cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(keys_cell), keys);
	gtk_size_group_add_widget(keys_column, keys_cell);
	gtk_box_append(GTK_BOX(r), keys_cell);
	GtkWidget *l = gtk_label_new(does.c_str());
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(l, TRUE);
	gtk_widget_set_tooltip_text(l, does.c_str());
	if (advanced)
		gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(r), l);
	return r;
}

void clear(GtkWidget *box) {
	while (GtkWidget *c = gtk_widget_get_first_child(box))
		gtk_box_remove(GTK_BOX(box), c);
}

/* Rows under a heading per category, in the catalog's order. */
void fill(GtkWidget *box, const std::map<std::string, std::vector<GtkWidget *>> &groups) {
	clear(box);
	for (const std::string &cat : category_order()) {
		auto it = groups.find(cat);
		if (it == groups.end())
			continue;
		GtkWidget *h = gtk_label_new(cat.c_str());
		gtk_label_set_xalign(GTK_LABEL(h), 0);
		gtk_widget_add_css_class(h, "wb-shortcut-group");
		gtk_box_append(GTK_BOX(box), h);
		for (GtkWidget *w : it->second)
			gtk_box_append(GTK_BOX(box), w);
	}
	if (gtk_widget_get_first_child(box) == nullptr)
		gtk_box_append(GTK_BOX(box), dim(_("None in rc.xml.")));
}

}  // namespace

GtkWidget *build_keybindings_block(Ui *ui) {
	ui->shortcut_keys = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	return ui->shortcut_keys;
}

GtkWidget *build_mousebindings_block(Ui *ui) {
	ui->shortcut_mouse = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	return ui->shortcut_mouse;
}

void populate_shortcuts(Ui *ui, const wb::RcDocument *doc) {
	if (doc == nullptr)
		return;
	ui->shortcut_lines.clear();
	if (ui->shortcut_keys != nullptr) {
		GtkSizeGroup *column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
		std::map<std::string, std::vector<GtkWidget *>> groups;
		for (const wb::KeyBinding &k : doc->keybindings()) {
			/* A chain: its keys' caps, "then" between them. */
			GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			for (size_t i = 0; i < k.chain.size(); ++i) {
				if (i > 0)
					gtk_box_append(GTK_BOX(keys), dim(_("then")));
				gtk_box_append(GTK_BOX(keys), caps(wb::key_caps(k.chain[i])));
			}
			bool adv = std::any_of(k.actions.begin(), k.actions.end(),
					[](const wb::BoundAction &a) { return a.advanced; });
			groups[category_of(k.actions)].push_back(
					row(column, keys, wb::describe_actions(k.actions), adv));
			std::string chain;
			for (const std::string &key : k.chain)
				chain += (chain.empty() ? "" : " ") + key;
			ui->shortcut_lines.push_back("key|" + chain + "|" +
					wb::describe_actions(k.actions) + "|" + category_of(k.actions));
		}
		fill(ui->shortcut_keys, groups);
		g_object_unref(column);
	}
	if (ui->shortcut_mouse != nullptr) {
		GtkSizeGroup *column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
		std::map<std::string, std::vector<GtkWidget *>> groups;
		for (const wb::MouseBinding &m : doc->mousebindings()) {
			/* Where, the button (with its modifiers), how: "Titlebar  [Left
			 * button]  double-click". */
			GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			GtkWidget *where = gtk_label_new(wb::context_label(m.context).c_str());
			gtk_widget_add_css_class(where, "wb-shortcut-place");
			gtk_box_append(GTK_BOX(keys), where);
			gtk_box_append(GTK_BOX(keys), caps(wb::button_caps(m.button)));
			std::string how = wb::event_label(m.event);
			if (!how.empty())
				how[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(how[0])));
			/* The wheel: scrolling says it all ("click" adds nothing). */
			bool wheel = m.button.ends_with("Up") || m.button.ends_with("Down");
			if (!wheel)
				gtk_box_append(GTK_BOX(keys), dim(how));
			bool adv = std::any_of(m.actions.begin(), m.actions.end(),
					[](const wb::BoundAction &a) { return a.advanced; });
			groups[category_of(m.actions)].push_back(
					row(column, keys, wb::describe_actions(m.actions), adv));
			ui->shortcut_lines.push_back("mouse|" + m.context + " " + m.button + " " + m.event +
					"|" + wb::describe_actions(m.actions) + "|" + category_of(m.actions));
		}
		fill(ui->shortcut_mouse, groups);
		g_object_unref(column);
	}
}

}  // namespace wbconf
