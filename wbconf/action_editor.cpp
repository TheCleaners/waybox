#include "action_editor.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <libintl.h>
#ifndef _
#define _(s) gettext(s)
#endif

#include "waybox/settings_schema.hpp"

namespace wbconf {

namespace {

struct ActionEditor {
	wb::BoundAction action;
	std::function<void(const wb::BoundAction &)> changed;
	GtkSizeGroup *labels = nullptr;
	GtkWidget *box = nullptr, *dropdown = nullptr, *options = nullptr;
	std::vector<std::string> names;  /* the dropdown's, in order */
	std::map<std::string, GtkWidget *> option_widgets;
	bool updating = false;
};

ActionEditor *of(GtkWidget *box) {
	return static_cast<ActionEditor *>(g_object_get_data(G_OBJECT(box), "wb-action-editor"));
}

GtkWidget *field(GtkSizeGroup *labels, const char *label, GtkWidget *w) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *l = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_size_group_add_widget(labels, l);
	gtk_box_append(GTK_BOX(row), l);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(row), w);
	return row;
}

/* An option's value; "command" also reads Openbox's older <execute>. */
std::string option_value(const wb::BoundAction &a, const std::string &name) {
	for (const auto &[k, v] : a.options)
		if (k == name || (name == "command" && k == "execute"))
			return v;
	return "";
}

void set_option(ActionEditor *e, const std::string &name, const std::string &value) {
	auto &opts = e->action.options;
	auto it = std::find_if(opts.begin(), opts.end(), [&](const auto &kv) {
		return kv.first == name || (name == "command" && kv.first == "execute");
	});
	if (value.empty()) {
		if (it != opts.end())
			opts.erase(it);
	} else if (it != opts.end()) {
		it->second = value;
	} else {
		opts.emplace_back(name, value);
	}
	if (e->changed)
		e->changed(e->action);
}

void build_options(ActionEditor *e) {
	while (GtkWidget *c = gtk_widget_get_first_child(e->options))
		gtk_box_remove(GTK_BOX(e->options), c);
	e->option_widgets.clear();
	const wb::ActionInfo *info = wb::action_info(e->action.name);
	if (info == nullptr)
		return;
	e->updating = true;
	for (const wb::ActionOption &o : info->options) {
		std::string v = option_value(e->action, o.name);
		GtkWidget *w = nullptr;
		const char *label = o.label.c_str();
		switch (o.kind) {
		case wb::OptionKind::Text:
		case wb::OptionKind::Number:
		case wb::OptionKind::Command:
			w = gtk_entry_new();
			gtk_editable_set_text(GTK_EDITABLE(w), v.c_str());
			if (!o.fallback.empty())
				gtk_entry_set_placeholder_text(GTK_ENTRY(w), o.fallback.c_str());
			if (o.kind == wb::OptionKind::Command) {
				gtk_widget_add_css_class(w, "monospace");
				gtk_editable_set_width_chars(GTK_EDITABLE(w), 40);
				gtk_entry_set_placeholder_text(GTK_ENTRY(w), _("A command, as in a terminal"));
			} else {
				gtk_editable_set_width_chars(GTK_EDITABLE(w), 12);
			}
			if (o.kind == wb::OptionKind::Number)
				gtk_entry_set_input_purpose(GTK_ENTRY(w), GTK_INPUT_PURPOSE_DIGITS);
			g_signal_connect(w, "changed", G_CALLBACK(+[](GtkEditable *ed, gpointer d) {
				auto *e = static_cast<ActionEditor *>(d);
				if (!e->updating)
					set_option(e, static_cast<const char *>(g_object_get_data(G_OBJECT(ed),
							"wb-option")), gtk_editable_get_text(ed));
			}), e);
			break;
		case wb::OptionKind::Choice: {
			std::vector<const char *> names;
			guint sel = 0;
			std::string want = v.empty() ? o.fallback : v;
			for (size_t i = 0; i < o.choices.size(); ++i) {
				names.push_back(o.choices[i].second.c_str());
				if (o.choices[i].first == want)
					sel = static_cast<guint>(i);
			}
			names.push_back(nullptr);
			w = gtk_drop_down_new_from_strings(names.data());
			gtk_drop_down_set_selected(GTK_DROP_DOWN(w), sel);
			g_signal_connect(w, "notify::selected", G_CALLBACK(+[](GObject *dd, GParamSpec *,
					gpointer d) {
				auto *e = static_cast<ActionEditor *>(d);
				if (e->updating)
					return;
				std::string name = static_cast<const char *>(g_object_get_data(dd, "wb-option"));
				const wb::ActionInfo *info = wb::action_info(e->action.name);
				guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
				if (info != nullptr)
					for (const wb::ActionOption &o : info->options)
						if (o.name == name && i < o.choices.size())
							set_option(e, name, o.choices[i].first);
			}), e);
			break;
		}
		case wb::OptionKind::Bool:
			w = gtk_check_button_new_with_label(o.label.c_str());
			gtk_check_button_set_active(GTK_CHECK_BUTTON(w),
					wb::setting_true(v.empty() ? o.fallback : v));
			g_signal_connect(w, "toggled", G_CALLBACK(+[](GtkCheckButton *b, gpointer d) {
				auto *e = static_cast<ActionEditor *>(d);
				if (!e->updating)
					set_option(e, static_cast<const char *>(g_object_get_data(G_OBJECT(b),
							"wb-option")), gtk_check_button_get_active(b) ? "yes" : "no");
			}), e);
			label = "";
			break;
		}
		g_object_set_data_full(G_OBJECT(w), "wb-option", g_strdup(o.name.c_str()), g_free);
		gtk_box_append(GTK_BOX(e->options), field(e->labels, label, w));
		e->option_widgets[o.name] = w;
	}
	e->updating = false;
}

void on_selected(GObject *, GParamSpec *, gpointer data) {
	auto *e = static_cast<ActionEditor *>(data);
	if (e->updating)
		return;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->dropdown));
	if (i >= e->names.size() || e->action.name == e->names[i])
		return;
	/* Options the new action has too are kept (a command, a desktop). */
	const wb::ActionInfo *info = wb::action_info(e->names[i]);
	std::vector<std::pair<std::string, std::string>> kept;
	if (info != nullptr)
		for (const auto &[k, v] : e->action.options)
			for (const wb::ActionOption &o : info->options)
				if (o.name == k || (o.name == "command" && k == "execute"))
					kept.emplace_back(k, v);
	e->action = {e->names[i], kept, false};
	build_options(e);
	if (e->changed)
		e->changed(e->action);
}

}  // namespace

GtkWidget *action_editor_new(GtkSizeGroup *labels, const wb::BoundAction &action,
		std::function<void(const wb::BoundAction &)> changed) {
	auto *e = new ActionEditor;
	e->action = action;
	e->changed = std::move(changed);
	e->labels = labels;
	e->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_object_set_data_full(G_OBJECT(e->box), "wb-action-editor", e,
			[](gpointer p) { delete static_cast<ActionEditor *>(p); });

	GtkStringList *model = gtk_string_list_new(nullptr);
	guint sel = GTK_INVALID_LIST_POSITION;
	const wb::ActionInfo *cur = wb::action_info(action.name);
	for (const wb::ActionInfo &a : wb::action_catalog()) {
		if (cur != nullptr && a.name == cur->name)
			sel = static_cast<guint>(e->names.size());
		e->names.push_back(a.name);
		gtk_string_list_append(model, a.label.c_str());
	}
	if (sel == GTK_INVALID_LIST_POSITION && !action.name.empty()) {
		sel = static_cast<guint>(e->names.size());
		e->names.push_back(action.name);
		gtk_string_list_append(model, action.name.c_str());
	}
	GtkExpression *expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
	e->dropdown = gtk_drop_down_new(G_LIST_MODEL(model), expr);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(e->dropdown), TRUE);
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(e->dropdown),
			GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
	if (sel != GTK_INVALID_LIST_POSITION)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(e->dropdown), sel);
	g_signal_connect(e->dropdown, "notify::selected", G_CALLBACK(on_selected), e);
	gtk_box_append(GTK_BOX(e->box), field(labels, _("Action"), e->dropdown));
	e->options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_append(GTK_BOX(e->box), e->options);
	build_options(e);
	return e->box;
}

void action_editor_select(GtkWidget *editor, const std::string &name) {
	ActionEditor *e = of(editor);
	if (e == nullptr)
		return;
	for (size_t i = 0; i < e->names.size(); ++i)
		if (e->names[i] == name)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(e->dropdown), static_cast<guint>(i));
}

void action_editor_set_option(GtkWidget *editor, const std::string &option,
		const std::string &value) {
	ActionEditor *e = of(editor);
	if (e == nullptr)
		return;
	auto it = e->option_widgets.find(option);
	if (it != e->option_widgets.end() && GTK_IS_EDITABLE(it->second))
		gtk_editable_set_text(GTK_EDITABLE(it->second), value.c_str());
}

}  // namespace wbconf
