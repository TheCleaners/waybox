/*
 * The Shortcuts page (#236): rc.xml's key and mouse bindings, as key caps
 * and a line saying what each does, grouped as the action catalog groups
 * actions.
 *
 * A key binding is edited where it stands: its row opens underneath with
 * the keys (click them, press the new ones), the action (a searchable
 * list) and that action's options, if it has any. Each row keeps the
 * binding as saved and as it is now; Save & Apply writes the ones that
 * differ. Bindings with If, ForEach and the like stay rc.xml's to edit.
 * Mouse bindings are listed, not edited yet.
 */
#include "wbconf_ui.hpp"

#include <algorithm>
#include <map>

#include "waybox/action_catalog.hpp"
#include "waybox/key_label.hpp"

namespace wbconf {

struct ShortcutEdit {
	Ui *ui = nullptr;
	std::optional<wb::KeyBinding> was, now;
	bool editable = true;
	GtkWidget *outer = nullptr;      /* the row and, under it, its editor */
	GtkWidget *keys_cell = nullptr;  /* in the column of keys */
	GtkWidget *does = nullptr;
	GtkWidget *head = nullptr;
	GtkWidget *undo = nullptr;
	GtkWidget *revealer = nullptr;
	GtkWidget *keys_button = nullptr;
	GtkWidget *keys_hint = nullptr;
	GtkWidget *then_button = nullptr;
	GtkWidget *action = nullptr;     /* GtkDropDown */
	GtkWidget *options = nullptr;
	GtkWidget *also = nullptr;
	std::vector<std::string> action_names;  /* the dropdown's, in order */
	std::map<std::string, GtkWidget *> option_widgets;
	bool append = false;  /* capturing the next key of a chain */
	bool updating = false;
};

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

bool any_advanced(const std::vector<wb::BoundAction> &actions) {
	return std::any_of(actions.begin(), actions.end(),
			[](const wb::BoundAction &a) { return a.advanced; });
}

std::string chain_text(const std::vector<std::string> &chain) {
	std::string out;
	for (const std::string &k : chain)
		out += (out.empty() ? "" : " ") + k;
	return out;
}

/* "Super D then X": a binding's keys as read. */
std::string chain_caps_text(const std::vector<std::string> &chain) {
	std::string out;
	for (const std::string &k : chain) {
		if (!out.empty())
			out += std::string(" ") + _("then") + " ";
		std::string step;
		for (const std::string &c : wb::key_caps(k))
			step += (step.empty() ? "" : "+") + c;
		out += step;
	}
	return out;
}

GtkWidget *cap(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "wb-keycap");
	return l;
}

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

void clear(GtkWidget *box) {
	while (GtkWidget *c = gtk_widget_get_first_child(box))
		gtk_box_remove(GTK_BOX(box), c);
}

/* A chain's caps, "then" between its keys. */
GtkWidget *chain_caps(const std::vector<std::string> &chain) {
	GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	for (size_t i = 0; i < chain.size(); ++i) {
		if (i > 0)
			gtk_box_append(GTK_BOX(keys), dim(_("then")));
		gtk_box_append(GTK_BOX(keys), caps(wb::key_caps(chain[i])));
	}
	return keys;
}

/* ---- One key binding's row ---- */

const wb::KeyBinding &shown(const ShortcutEdit *e) {
	return e->now ? *e->now : *e->was;
}

/* The row's keys and line, as the binding is now (or was, struck out). */
void refresh_head(ShortcutEdit *e) {
	const wb::KeyBinding &b = shown(e);
	clear(e->keys_cell);
	gtk_box_append(GTK_BOX(e->keys_cell),
			b.chain.empty() ? dim(_("No keys yet")) : chain_caps(b.chain));
	std::string does = b.actions.empty() ? std::string(_("Nothing")) : wb::describe_actions(b.actions);
	gtk_label_set_text(GTK_LABEL(e->does), does.c_str());
	gtk_widget_set_tooltip_text(e->does, does.c_str());
	bool removed = !e->now;
	if (removed)
		gtk_widget_add_css_class(e->head, "wb-removed");
	else
		gtk_widget_remove_css_class(e->head, "wb-removed");
	if (e->now != e->was)
		gtk_widget_add_css_class(e->outer, "wb-shortcut-unsaved");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-unsaved");
	gtk_widget_set_visible(e->undo, e->now != e->was && e->was.has_value());
	gtk_widget_set_tooltip_text(e->undo, removed ? _("Keep this shortcut")
			: _("Put back the shortcut as saved"));
}

/* The keys in the editor: the caps, or the prompt while capturing. */
void refresh_keys(ShortcutEdit *e) {
	if (e->keys_button == nullptr || !e->now)
		return;
	bool capturing = e->ui->capturing == e;
	GtkWidget *inside;
	if (capturing)
		inside = gtk_label_new(e->append ? _("Press the next key…") : _("Press the new keys…"));
	else if (e->now->chain.empty())
		inside = gtk_label_new(_("Click, then press the keys"));
	else
		inside = chain_caps(e->now->chain);
	gtk_button_set_child(GTK_BUTTON(e->keys_button), inside);
	if (capturing)
		gtk_widget_add_css_class(e->keys_button, "wb-capturing");
	else
		gtk_widget_remove_css_class(e->keys_button, "wb-capturing");
	gtk_widget_set_visible(e->keys_hint, capturing);
	gtk_widget_set_visible(e->then_button, !capturing && !e->now->chain.empty() &&
			e->now->chain.size() < 4);
}

void changed(ShortcutEdit *e) {
	refresh_head(e);
	refresh_keys(e);
	refresh_save_state(e->ui);
}

/* ---- Capturing keys ---- */

GdkToplevel *toplevel(Ui *ui) {
	if (ui->window == nullptr)
		return nullptr;
	GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(ui->window));
	return s != nullptr && GDK_IS_TOPLEVEL(s) ? GDK_TOPLEVEL(s) : nullptr;
}

void end_capture(Ui *ui) {
	ShortcutEdit *e = ui->capturing;
	if (e == nullptr)
		return;
	ui->capturing = nullptr;
	if (GdkToplevel *t = toplevel(ui))
		gdk_toplevel_restore_system_shortcuts(t);
	refresh_keys(e);
}

/* The key pressed, unshifted: the level-0 keyval of its key in the
 * layout in use ("1" for Shift+1, not "exclam"), so Shift reads as S-. */
std::string base_key_name(GdkEvent *ev, guint keyval) {
	GdkDisplay *d = gdk_event_get_display(ev);
	guint code = gdk_key_event_get_keycode(ev);
	guint layout = gdk_key_event_get_layout(ev);
	GdkKeymapKey *keys = nullptr;
	guint *vals = nullptr;
	int n = 0;
	guint base = keyval;
	if (gdk_display_map_keycode(d, code, &keys, &vals, &n)) {
		for (int i = 0; i < n; ++i)
			if (keys[i].group == static_cast<int>(layout) && keys[i].level == 0) {
				base = vals[i];
				break;
			}
		g_free(keys);
		g_free(vals);
	}
	const char *name = gdk_keyval_name(base);
	return name != nullptr ? name : "";
}

gboolean on_capture_key(GtkEventControllerKey *ctl, guint keyval, guint, GdkModifierType state,
		gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	ShortcutEdit *e = ui->capturing;
	if (e == nullptr || !e->now)
		return FALSE;
	GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctl));
	wb::HeldModifiers held;
	held.super = (state & GDK_SUPER_MASK) != 0;
	held.alt = (state & GDK_ALT_MASK) != 0;
	held.ctrl = (state & GDK_CONTROL_MASK) != 0;
	held.shift = (state & GDK_SHIFT_MASK) != 0;
	std::string name = ev != nullptr ? base_key_name(ev, keyval) : gdk_keyval_name(keyval);
	if (name == "Escape" && !held.super && !held.alt && !held.ctrl && !held.shift) {
		end_capture(ui);  /* keys as they were */
		return TRUE;
	}
	std::string key = wb::rc_key(held, name);
	if (key.empty())
		return TRUE;  /* a modifier, on its own so far */
	if (e->append)
		e->now->chain.push_back(key);
	else
		e->now->chain = {key};
	end_capture(ui);
	changed(e);
	return TRUE;
}

void start_capture(ShortcutEdit *e, bool append) {
	Ui *ui = e->ui;
	end_capture(ui);
	if (ui->capture_keys == nullptr && ui->window != nullptr) {
		/* On the window, before anything in it: every key goes here while
		 * capturing, Tab and Enter too. */
		GtkEventController *k = gtk_event_controller_key_new();
		gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
		g_signal_connect(k, "key-pressed", G_CALLBACK(on_capture_key), ui);
		gtk_widget_add_controller(GTK_WIDGET(ui->window), k);
		ui->capture_keys = k;
	}
	ui->capturing = e;
	e->append = append;
	/* Waybox's own shortcuts (Super+D, ...) pass through to be pressed
	 * here; Super+Escape still takes them back. */
	if (GdkToplevel *t = toplevel(ui))
		gdk_toplevel_inhibit_system_shortcuts(t, nullptr);
	refresh_keys(e);
}

/* ---- The editor ---- */

wb::BoundAction &first_action(ShortcutEdit *e) {
	if (e->now->actions.empty())
		e->now->actions.push_back({"Execute", {}, false});
	return e->now->actions[0];
}

/* An option's value now; "command" also reads Openbox's older <execute>. */
std::string option_value(const wb::BoundAction &a, const std::string &name) {
	for (const auto &[k, v] : a.options)
		if (k == name || (name == "command" && k == "execute"))
			return v;
	return "";
}

void set_option(ShortcutEdit *e, const std::string &name, const std::string &value) {
	auto &opts = first_action(e).options;
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
	changed(e);
}

void on_option_text(GtkEditable *ed, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->updating)
		return;
	const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(ed), "wb-option"));
	set_option(e, name, gtk_editable_get_text(ed));
}

void on_option_choice(GObject *dd, GParamSpec *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->updating)
		return;
	const char *name = static_cast<const char *>(g_object_get_data(dd, "wb-option"));
	const wb::ActionInfo *info = wb::action_info(first_action(e).name);
	if (info == nullptr)
		return;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	for (const wb::ActionOption &o : info->options)
		if (o.name == name && i < o.choices.size())
			set_option(e, name, o.choices[i].first);
}

void on_option_bool(GtkCheckButton *b, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->updating)
		return;
	const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-option"));
	set_option(e, name, gtk_check_button_get_active(b) ? "yes" : "no");
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

/* The chosen action's options: only those it has. */
void build_options(ShortcutEdit *e) {
	clear(e->options);
	e->option_widgets.clear();
	const wb::BoundAction &a = first_action(e);
	const wb::ActionInfo *info = wb::action_info(a.name);
	if (info == nullptr)
		return;
	GtkSizeGroup *labels = GTK_SIZE_GROUP(g_object_get_data(G_OBJECT(e->options), "wb-labels"));
	e->updating = true;
	for (const wb::ActionOption &o : info->options) {
		std::string v = option_value(a, o.name);
		GtkWidget *w = nullptr;
		switch (o.kind) {
		case wb::OptionKind::Text:
		case wb::OptionKind::Number:
		case wb::OptionKind::Command: {
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
			g_object_set_data_full(G_OBJECT(w), "wb-option", g_strdup(o.name.c_str()), g_free);
			g_signal_connect(w, "changed", G_CALLBACK(on_option_text), e);
			break;
		}
		case wb::OptionKind::Choice: {
			std::vector<const char *> labels_v;
			guint sel = 0;
			std::string want = v.empty() ? o.fallback : v;
			for (size_t i = 0; i < o.choices.size(); ++i) {
				labels_v.push_back(o.choices[i].second.c_str());
				if (o.choices[i].first == want)
					sel = static_cast<guint>(i);
			}
			labels_v.push_back(nullptr);
			w = gtk_drop_down_new_from_strings(labels_v.data());
			gtk_drop_down_set_selected(GTK_DROP_DOWN(w), sel);
			g_object_set_data_full(G_OBJECT(w), "wb-option", g_strdup(o.name.c_str()), g_free);
			g_signal_connect(w, "notify::selected", G_CALLBACK(on_option_choice), e);
			break;
		}
		case wb::OptionKind::Bool: {
			w = gtk_check_button_new_with_label(o.label.c_str());
			std::string want = v.empty() ? o.fallback : v;
			gtk_check_button_set_active(GTK_CHECK_BUTTON(w), wb::setting_true(want));
			g_object_set_data_full(G_OBJECT(w), "wb-option", g_strdup(o.name.c_str()), g_free);
			g_signal_connect(w, "toggled", G_CALLBACK(on_option_bool), e);
			gtk_box_append(GTK_BOX(e->options), field(labels, "", w));
			e->option_widgets[o.name] = w;
			continue;
		}
		}
		gtk_box_append(GTK_BOX(e->options), field(labels, o.label.c_str(), w));
		e->option_widgets[o.name] = w;
	}
	e->updating = false;
	/* More than one action: the first edited here, the rest kept. */
	if (e->now->actions.size() > 1) {
		std::vector<wb::BoundAction> rest(e->now->actions.begin() + 1, e->now->actions.end());
		gtk_label_set_text(GTK_LABEL(e->also),
				(std::string(_("Then also: ")) + wb::describe_actions(rest)).c_str());
		gtk_widget_set_visible(e->also, TRUE);
	} else {
		gtk_widget_set_visible(e->also, FALSE);
	}
}

void on_action_selected(GObject *, GParamSpec *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->updating || !e->now)
		return;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->action));
	if (i >= e->action_names.size())
		return;
	wb::BoundAction &a = first_action(e);
	if (a.name == e->action_names[i])
		return;
	/* Options the new action has too are kept (a command, a desktop). */
	const wb::ActionInfo *info = wb::action_info(e->action_names[i]);
	std::vector<std::pair<std::string, std::string>> kept;
	if (info != nullptr)
		for (const auto &[k, v] : a.options)
			for (const wb::ActionOption &o : info->options)
				if (o.name == k || (o.name == "command" && k == "execute"))
					kept.emplace_back(k, v);
	a = {e->action_names[i], kept, false};
	build_options(e);
	changed(e);
}

void on_keys_clicked(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->ui->capturing == e)
		end_capture(e->ui);
	else
		start_capture(e, false);
}

void on_then_clicked(GtkButton *, gpointer data) {
	start_capture(static_cast<ShortcutEdit *>(data), true);
}

void set_open(ShortcutEdit *e, bool open) {
	if (!open && e->ui->capturing == e)
		end_capture(e->ui);
	gtk_revealer_set_reveal_child(GTK_REVEALER(e->revealer), open);
	if (open)
		gtk_widget_add_css_class(e->outer, "wb-shortcut-open");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-open");
}

void on_head_clicked(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (!e->now)
		return;
	set_open(e, !gtk_revealer_get_reveal_child(GTK_REVEALER(e->revealer)));
}

void on_done(GtkButton *, gpointer data) {
	set_open(static_cast<ShortcutEdit *>(data), false);
}

void forget(ShortcutEdit *e) {
	Ui *ui = e->ui;
	if (ui->capturing == e)
		end_capture(ui);
	gtk_box_remove(GTK_BOX(gtk_widget_get_parent(e->outer)), e->outer);
	auto it = std::find_if(ui->shortcut_edits.begin(), ui->shortcut_edits.end(),
			[e](const auto &p) { return p.get() == e; });
	if (it != ui->shortcut_edits.end())
		ui->shortcut_edits.erase(it);  /* e is gone from here on */
	refresh_save_state(ui);
}

void on_remove(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	set_open(e, false);
	if (!e->was) {
		forget(e);  /* never saved: nothing to keep */
		return;
	}
	e->now.reset();
	changed(e);
}

void on_undo(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	set_open(e, false);
	e->now = e->was;
	/* The editor shows the saved binding again. */
	e->updating = true;
	for (size_t i = 0; i < e->action_names.size(); ++i)
		if (e->now && !e->now->actions.empty() && e->action_names[i] == e->now->actions[0].name)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(e->action), static_cast<guint>(i));
	e->updating = false;
	if (e->now)
		build_options(e);
	changed(e);
}

GtkWidget *build_editor(ShortcutEdit *e) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "wb-shortcut-editor");
	GtkSizeGroup *labels = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	/* Keys: click, press. */
	GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	e->keys_button = gtk_button_new();
	gtk_widget_add_css_class(e->keys_button, "wb-keys-button");
	gtk_widget_set_tooltip_text(e->keys_button, _("Click, then press the keys to use"));
	g_signal_connect(e->keys_button, "clicked", G_CALLBACK(on_keys_clicked), e);
	gtk_box_append(GTK_BOX(keys), e->keys_button);
	e->then_button = gtk_button_new_with_label(_("Then another key"));
	gtk_widget_add_css_class(e->then_button, "flat");
	gtk_widget_set_tooltip_text(e->then_button,
			_("A chain: press these keys, let go, then the next one"));
	g_signal_connect(e->then_button, "clicked", G_CALLBACK(on_then_clicked), e);
	gtk_box_append(GTK_BOX(keys), e->then_button);
	e->keys_hint = dim(_("Esc cancels"));
	gtk_box_append(GTK_BOX(keys), e->keys_hint);
	gtk_box_append(GTK_BOX(box), field(labels, _("Keys"), keys));

	/* Action: type to find it. */
	GtkStringList *model = gtk_string_list_new(nullptr);
	guint sel = GTK_INVALID_LIST_POSITION;
	std::string current = e->now->actions.empty() ? "" : e->now->actions[0].name;
	const wb::ActionInfo *cur = wb::action_info(current);
	for (const wb::ActionInfo &a : wb::action_catalog()) {
		if (cur != nullptr && a.name == cur->name)
			sel = static_cast<guint>(e->action_names.size());
		e->action_names.push_back(a.name);
		gtk_string_list_append(model, a.label.c_str());
	}
	if (sel == GTK_INVALID_LIST_POSITION && !current.empty()) {
		/* An action the catalog doesn't list: shown by its name. */
		sel = static_cast<guint>(e->action_names.size());
		e->action_names.push_back(current);
		gtk_string_list_append(model, current.c_str());
	}
	GtkExpression *expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
	e->action = gtk_drop_down_new(G_LIST_MODEL(model), expr);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(e->action), TRUE);
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(e->action), GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
	if (sel != GTK_INVALID_LIST_POSITION)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(e->action), sel);
	g_signal_connect(e->action, "notify::selected", G_CALLBACK(on_action_selected), e);
	gtk_box_append(GTK_BOX(box), field(labels, _("Action"), e->action));

	e->options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_object_set_data_full(G_OBJECT(e->options), "wb-labels", labels, g_object_unref);
	gtk_box_append(GTK_BOX(box), e->options);
	e->also = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->also), 0);
	gtk_label_set_wrap(GTK_LABEL(e->also), TRUE);
	gtk_widget_add_css_class(e->also, "dim-label");
	gtk_box_append(GTK_BOX(box), e->also);
	build_options(e);

	/* Remove on the left, away from Done. */
	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(foot, 4);
	GtkWidget *remove = gtk_button_new_with_label(_("Remove Shortcut"));
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_add_css_class(remove, "destructive-action");
	g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), e);
	gtk_box_append(GTK_BOX(foot), remove);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	GtkWidget *done = gtk_button_new_with_label(_("Done"));
	g_signal_connect(done, "clicked", G_CALLBACK(on_done), e);
	gtk_box_append(GTK_BOX(foot), done);
	gtk_box_append(GTK_BOX(box), foot);
	refresh_keys(e);
	return box;
}

/* A key binding's row: a button that opens its editor underneath, or for
 * one only rc.xml edits, a quiet line saying so. */
ShortcutEdit *key_row(Ui *ui, GtkSizeGroup *column, std::optional<wb::KeyBinding> was,
		std::optional<wb::KeyBinding> now) {
	auto e = std::make_shared<ShortcutEdit>();
	e->ui = ui;
	e->was = std::move(was);
	e->now = std::move(now);
	e->editable = !any_advanced(shown(e.get()).actions);
	e->outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(e->outer, "wb-shortcut");

	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	e->keys_cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_size_group_add_widget(column, e->keys_cell);
	gtk_box_append(GTK_BOX(line), e->keys_cell);
	e->does = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->does), 0);
	gtk_label_set_ellipsize(GTK_LABEL(e->does), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(e->does, TRUE);
	gtk_box_append(GTK_BOX(line), e->does);

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	if (e->editable) {
		e->head = gtk_button_new();
		gtk_button_set_child(GTK_BUTTON(e->head), line);
		gtk_widget_add_css_class(e->head, "flat");
		gtk_widget_add_css_class(e->head, "wb-shortcut-row");
		gtk_widget_set_hexpand(e->head, TRUE);
		g_signal_connect(e->head, "clicked", G_CALLBACK(on_head_clicked), e.get());
	} else {
		e->head = line;
		gtk_widget_add_css_class(e->head, "wb-shortcut-row");
		gtk_widget_add_css_class(e->does, "dim-label");
		gtk_widget_set_tooltip_text(e->head,
				_("Uses If, ForEach or the like: edited in rc.xml"));
	}
	gtk_box_append(GTK_BOX(bar), e->head);
	e->undo = gtk_button_new_from_icon_name("edit-undo-symbolic");
	gtk_widget_add_css_class(e->undo, "flat");
	gtk_widget_set_valign(e->undo, GTK_ALIGN_CENTER);
	g_signal_connect(e->undo, "clicked", G_CALLBACK(on_undo), e.get());
	gtk_box_append(GTK_BOX(bar), e->undo);
	gtk_box_append(GTK_BOX(e->outer), bar);

	e->revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(e->revealer),
			GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(e->revealer), 150);
	if (e->editable && e->now)
		gtk_revealer_set_child(GTK_REVEALER(e->revealer), build_editor(e.get()));
	gtk_box_append(GTK_BOX(e->outer), e->revealer);
	refresh_head(e.get());
	ui->shortcut_edits.push_back(e);
	return e.get();
}

void on_add(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	wb::KeyBinding fresh{{}, {{"Execute", {}, false}}};
	ShortcutEdit *e = key_row(ui, ui->shortcut_column, std::nullopt, fresh);
	gtk_box_insert_child_after(GTK_BOX(ui->shortcut_keys), e->outer,
			gtk_widget_get_prev_sibling(ui->shortcut_add));
	set_open(e, true);
	start_capture(e, false);
	refresh_save_state(ui);
}

/* Rows under a heading per category, in the catalog's order. */
void fill(GtkWidget *box, const std::map<std::string, std::vector<GtkWidget *>> &groups) {
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
}

ShortcutEdit *find_edit(Ui *ui, const std::string &chain) {
	for (auto &e : ui->shortcut_edits)
		if (chain_text(shown(e.get()).chain) == chain)
			return e.get();
	return nullptr;
}

std::vector<std::string> split_chain(const std::string &s) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at <= s.size()) {
		size_t sp = s.find(' ', at);
		std::string k = s.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
		if (!k.empty())
			out.push_back(k);
		if (sp == std::string::npos)
			break;
		at = sp + 1;
	}
	return out;
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
	end_capture(ui);
	ui->shortcut_lines.clear();
	if (ui->shortcut_keys != nullptr) {
		clear(ui->shortcut_keys);
		ui->shortcut_edits.clear();
		if (ui->shortcut_column != nullptr)
			g_object_unref(ui->shortcut_column);
		ui->shortcut_column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
		std::map<std::string, std::vector<GtkWidget *>> groups;
		for (const wb::KeyBinding &k : doc->keybindings()) {
			ShortcutEdit *e = key_row(ui, ui->shortcut_column, k, k);
			groups[category_of(k.actions)].push_back(e->outer);
		}
		fill(ui->shortcut_keys, groups);
		ui->shortcut_add = gtk_button_new();
		GtkWidget *add = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_append(GTK_BOX(add), gtk_image_new_from_icon_name("list-add-symbolic"));
		gtk_box_append(GTK_BOX(add), gtk_label_new(_("Add a Shortcut")));
		gtk_button_set_child(GTK_BUTTON(ui->shortcut_add), add);
		gtk_widget_add_css_class(ui->shortcut_add, "flat");
		gtk_widget_set_halign(ui->shortcut_add, GTK_ALIGN_START);
		gtk_widget_set_margin_top(ui->shortcut_add, 8);
		g_signal_connect(ui->shortcut_add, "clicked", G_CALLBACK(on_add), ui);
		gtk_box_append(GTK_BOX(ui->shortcut_keys), ui->shortcut_add);
	}
	if (ui->shortcut_mouse != nullptr) {
		clear(ui->shortcut_mouse);
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
			GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
			gtk_widget_add_css_class(r, "wb-shortcut-row");
			gtk_widget_add_css_class(r, "wb-shortcut-static");
			GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
			gtk_box_append(GTK_BOX(cell), keys);
			gtk_size_group_add_widget(column, cell);
			gtk_box_append(GTK_BOX(r), cell);
			std::string does = wb::describe_actions(m.actions);
			GtkWidget *l = gtk_label_new(does.c_str());
			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
			gtk_widget_set_hexpand(l, TRUE);
			gtk_widget_set_tooltip_text(l, does.c_str());
			if (any_advanced(m.actions))
				gtk_widget_add_css_class(l, "dim-label");
			gtk_box_append(GTK_BOX(r), l);
			groups[category_of(m.actions)].push_back(r);
			ui->shortcut_lines.push_back("mouse|" + m.context + " " + m.button + " " + m.event +
					"|" + does + "|" + category_of(m.actions));
		}
		fill(ui->shortcut_mouse, groups);
		if (gtk_widget_get_first_child(ui->shortcut_mouse) == nullptr)
			gtk_box_append(GTK_BOX(ui->shortcut_mouse), dim(_("None in rc.xml.")));
		g_object_unref(column);
	}
}

void apply_shortcuts(Ui *ui, wb::RcDocument &doc) {
	end_capture(ui);
	/* Keys given up first (removed or moved), so a binding moving onto
	 * keys another just left finds them free. */
	for (auto &e : ui->shortcut_edits)
		if (e->was && e->now != e->was && (!e->now || e->now->chain != e->was->chain))
			doc.set_keybinding(e->was->chain, std::nullopt);
	for (auto &e : ui->shortcut_edits) {
		if (!e->now || e->now == e->was || e->now->chain.empty())
			continue;
		bool same_keys = e->was && e->was->chain == e->now->chain;
		doc.set_keybinding(same_keys ? e->was->chain : std::vector<std::string>{}, e->now);
	}
}

void shortcut_changes(Ui *ui, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out) {
	auto text = [](const std::optional<wb::KeyBinding> &b) {
		if (!b)
			return std::string(_("none"));
		return chain_caps_text(b->chain) + "  " + wb::describe_actions(b->actions);
	};
	for (auto &e : ui->shortcut_edits) {
		if (e->now == e->was || (e->now && e->now->chain.empty()))
			continue;  /* the same, or new and not given keys yet */
		out.push_back({{where, text(e->was), text(e->now)}, target});
	}
}

void shortcut_dump(Ui *ui) {
	for (auto &e : ui->shortcut_edits) {
		const wb::KeyBinding &b = shown(e.get());
		std::string state = !e->now ? "|removed" : e->now != e->was ? "|changed" : "";
		fprintf(stderr, "shortcut:key|%s|%s|%s%s\n", chain_text(b.chain).c_str(),
				wb::describe_actions(b.actions).c_str(), category_of(b.actions).c_str(),
				state.c_str());
	}
	for (const std::string &l : ui->shortcut_lines)
		fprintf(stderr, "shortcut:%s\n", l.c_str());
}

/* Test steps, through the same widgets a person uses:
 *   shortcut-open:CHAIN            shortcut-capture:CHAIN
 *   shortcut-keys:CHAIN=NEWCHAIN   (as if pressed)
 *   shortcut-action:CHAIN=Name     shortcut-option:CHAIN=name=value
 *   shortcut-remove:CHAIN          shortcut-undo:CHAIN
 *   shortcut-add:CHAIN=Name        (a new binding)
 */
void shortcut_test_step(Ui *ui, const std::string &step) {
	size_t colon = step.find(':');
	std::string verb = step.substr(0, colon), arg = step.substr(colon + 1);
	size_t eq = arg.find('=');
	std::string chain = arg.substr(0, eq), rest = eq == std::string::npos ? "" : arg.substr(eq + 1);
	if (verb == "shortcut-add") {
		on_add(nullptr, ui);
		ShortcutEdit *e = ui->shortcut_edits.back().get();
		end_capture(ui);
		e->now->chain = split_chain(chain);
		changed(e);
		for (size_t i = 0; i < e->action_names.size(); ++i)
			if (e->action_names[i] == rest)
				gtk_drop_down_set_selected(GTK_DROP_DOWN(e->action), static_cast<guint>(i));
		return;
	}
	ShortcutEdit *e = find_edit(ui, chain);
	if (e == nullptr) {
		fprintf(stderr, "shortcut-step: no binding %s\n", chain.c_str());
		return;
	}
	if (verb == "shortcut-open") {
		set_open(e, true);
	} else if (verb == "shortcut-capture") {
		set_open(e, true);
		start_capture(e, false);
	} else if (verb == "shortcut-keys" && e->now) {
		e->now->chain = split_chain(rest);
		changed(e);
	} else if (verb == "shortcut-action") {
		for (size_t i = 0; i < e->action_names.size(); ++i)
			if (e->action_names[i] == rest)
				gtk_drop_down_set_selected(GTK_DROP_DOWN(e->action), static_cast<guint>(i));
	} else if (verb == "shortcut-option") {
		size_t eq2 = rest.find('=');
		auto it = e->option_widgets.find(rest.substr(0, eq2));
		if (it != e->option_widgets.end() && GTK_IS_EDITABLE(it->second))
			gtk_editable_set_text(GTK_EDITABLE(it->second), rest.substr(eq2 + 1).c_str());
	} else if (verb == "shortcut-remove") {
		on_remove(nullptr, e);
	} else if (verb == "shortcut-undo") {
		on_undo(nullptr, e);
	}
}

}  // namespace wbconf
