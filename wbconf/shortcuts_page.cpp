/*
 * The Shortcuts page (#236): rc.xml's key and mouse bindings, as key caps
 * and a line saying what each does, grouped as the action catalog groups
 * actions.
 *
 * A binding is edited where it stands: its row opens underneath with what
 * sets it off (keys: click them, press the new ones; mouse: where, which
 * button, with which keys held, how), the action (a searchable list) and
 * that action's options, if it has any. Each row keeps the binding as
 * saved and as it is now; Save & Apply writes the ones that differ.
 * Bindings with If, ForEach and the like stay rc.xml's to edit.
 *
 * Search filters the rows themselves, and offers to bind an action no key
 * runs yet ("Add a shortcut for Maximize or restore").
 */
#include "wbconf_ui.hpp"

#include <algorithm>
#include <map>

#include <fstream>
#include <sstream>

#include "waybox/action_catalog.hpp"
#include "waybox/binding_clash.hpp"
#include "waybox/key_label.hpp"

namespace wbconf {

struct ShortcutEdit {
	Ui *ui = nullptr;
	bool mouse = false;
	std::optional<wb::KeyBinding> was, now;      /* a key binding */
	std::optional<wb::MouseBinding> mwas, mnow;  /* or a mouse one */
	bool editable = true;
	GtkWidget *outer = nullptr;      /* the row and, under it, its editor */
	GtkWidget *heading = nullptr;    /* its category's, if listed under one */
	GtkWidget *keys_cell = nullptr;  /* in the column of keys */
	GtkWidget *does = nullptr;
	GtkWidget *head = nullptr;
	GtkWidget *undo = nullptr;
	GtkWidget *revealer = nullptr;
	/* Never runs: why, and the fix offered. */
	GtkWidget *warn = nullptr, *warn_label = nullptr, *warn_fix = nullptr;
	ShortcutEdit *clash_by = nullptr;  /* the binding that takes its keys */
	const char *clash_kind = "";       /* or "script", "place" (for tests) */
	/* Keys. */
	GtkWidget *keys_button = nullptr;
	GtkWidget *keys_hint = nullptr;
	GtkWidget *then_button = nullptr;
	/* Mouse. */
	GtkWidget *place = nullptr, *button = nullptr, *event = nullptr, *event_field = nullptr;
	std::vector<std::string> places, buttons, events;  /* the dropdowns' values */
	std::map<std::string, GtkWidget *> mods;           /* "W-" -> its toggle */
	/* The action. */
	GtkWidget *action = nullptr;     /* GtkDropDown */
	GtkWidget *options = nullptr;
	GtkWidget *also = nullptr;
	std::vector<std::string> action_names;  /* the dropdown's, in order */
	std::map<std::string, GtkWidget *> option_widgets;
	bool append = false;  /* capturing the next key of a chain */
	bool updating = false;
};

namespace {

/* ---- The binding, key or mouse ---- */

bool present(const ShortcutEdit *e) {
	return e->mouse ? e->mnow.has_value() : e->now.has_value();
}

bool dirty(const ShortcutEdit *e) {
	return e->mouse ? e->mnow != e->mwas : e->now != e->was;
}

bool was_saved(const ShortcutEdit *e) {
	return e->mouse ? e->mwas.has_value() : e->was.has_value();
}

/* New and not set off by anything yet: not a change to save. */
bool incomplete(const ShortcutEdit *e) {
	return !e->mouse && e->now && e->now->chain.empty();
}

std::vector<wb::BoundAction> &acts(ShortcutEdit *e) {
	return e->mouse ? e->mnow->actions : e->now->actions;
}

/* What it does, as now (or as it was, when removed). */
const std::vector<wb::BoundAction> &shown_acts(const ShortcutEdit *e) {
	if (e->mouse)
		return e->mnow ? e->mnow->actions : e->mwas->actions;
	return e->now ? e->now->actions : e->was->actions;
}

void forget_changes(ShortcutEdit *e) {
	if (e->mouse)
		e->mnow = e->mwas;
	else
		e->now = e->was;
}

void remove_now(ShortcutEdit *e) {
	if (e->mouse)
		e->mnow.reset();
	else
		e->now.reset();
}

/* ---- Text ---- */

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

std::string caps_text(const std::vector<std::string> &caps) {
	std::string out;
	for (const std::string &c : caps)
		out += (out.empty() ? "" : "+") + c;
	return out;
}

/* "Super+D then X": a binding's keys as read. */
std::string chain_caps_text(const std::vector<std::string> &chain) {
	std::string out;
	for (const std::string &k : chain) {
		if (!out.empty())
			out += std::string(" ") + _("then") + " ";
		out += caps_text(wb::key_caps(k));
	}
	return out;
}

std::string lower_first(std::string s) {
	if (!s.empty())
		s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
	return s;
}

bool is_wheel(const std::string &button) {
	return button.ends_with("Up") || button.ends_with("Down");
}

/* "Titlebar Left button double-click". */
std::string mouse_text(const wb::MouseBinding &m) {
	std::string out = wb::context_label(m.context) + " " + caps_text(wb::button_caps(m.button));
	if (!is_wheel(m.button))
		out += " " + lower_first(wb::event_label(m.event));
	return out;
}

/* The row's identity for tests and the dump: "W-a x", "Root Right Press". */
std::string id_of(const ShortcutEdit *e) {
	if (e->mouse) {
		const wb::MouseBinding &m = e->mnow ? *e->mnow : *e->mwas;
		return m.context + " " + m.button + " " + m.event;
	}
	return chain_text(e->now ? e->now->chain : e->was->chain);
}

/* ---- Small widgets ---- */

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

GtkWidget *chain_caps(const std::vector<std::string> &chain) {
	GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	for (size_t i = 0; i < chain.size(); ++i) {
		if (i > 0)
			gtk_box_append(GTK_BOX(keys), dim(_("then")));
		gtk_box_append(GTK_BOX(keys), caps(wb::key_caps(chain[i])));
	}
	return keys;
}

/* "Titlebar  [Left button]  double-click"; the wheel without "click". */
GtkWidget *mouse_caps(const wb::MouseBinding &m) {
	GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *where = gtk_label_new(wb::context_label(m.context).c_str());
	gtk_widget_add_css_class(where, "wb-shortcut-place");
	gtk_box_append(GTK_BOX(keys), where);
	gtk_box_append(GTK_BOX(keys), caps(wb::button_caps(m.button)));
	if (!is_wheel(m.button))
		gtk_box_append(GTK_BOX(keys), dim(lower_first(wb::event_label(m.event))));
	return keys;
}

GtkWidget *dropdown(const std::vector<std::pair<std::string, std::string>> &items,
		const std::string &current, std::vector<std::string> &values) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	guint sel = 0;
	bool found = false;
	for (const auto &[v, label] : items) {
		if (v == current) {
			sel = static_cast<guint>(values.size());
			found = true;
		}
		values.push_back(v);
		gtk_string_list_append(model, label.c_str());
	}
	if (!found && !current.empty()) {
		/* What rc.xml has that isn't offered: shown as it's written. */
		sel = static_cast<guint>(values.size());
		values.push_back(current);
		gtk_string_list_append(model, current.c_str());
	}
	GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(model), nullptr);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
	return dd;
}

/* ---- The row ---- */

void refresh_head(ShortcutEdit *e) {
	clear(e->keys_cell);
	if (e->mouse)
		gtk_box_append(GTK_BOX(e->keys_cell), mouse_caps(e->mnow ? *e->mnow : *e->mwas));
	else if ((e->now ? e->now->chain : e->was->chain).empty())
		gtk_box_append(GTK_BOX(e->keys_cell), dim(_("No keys yet")));
	else
		gtk_box_append(GTK_BOX(e->keys_cell), chain_caps(e->now ? e->now->chain : e->was->chain));
	const auto &a = shown_acts(e);
	std::string does = a.empty() ? std::string(_("Nothing")) : wb::describe_actions(a);
	gtk_label_set_text(GTK_LABEL(e->does), does.c_str());
	gtk_widget_set_tooltip_text(e->does, does.c_str());
	if (!present(e))
		gtk_widget_add_css_class(e->head, "wb-removed");
	else
		gtk_widget_remove_css_class(e->head, "wb-removed");
	if (dirty(e))
		gtk_widget_add_css_class(e->outer, "wb-shortcut-unsaved");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-unsaved");
	gtk_widget_set_visible(e->undo, dirty(e) && was_saved(e));
	gtk_widget_set_tooltip_text(e->undo, !present(e) ? _("Keep this shortcut")
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

void refresh_clashes(Ui *ui);

void changed(ShortcutEdit *e) {
	refresh_head(e);
	refresh_keys(e);
	refresh_clashes(e->ui);
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

gboolean on_capture_key(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state,
		gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	ShortcutEdit *e = ui->capturing;
	if (e == nullptr || !e->now)
		return FALSE;
	wb::HeldModifiers held;
	held.super = (state & GDK_SUPER_MASK) != 0;
	held.alt = (state & GDK_ALT_MASK) != 0;
	held.ctrl = (state & GDK_CONTROL_MASK) != 0;
	held.shift = (state & GDK_SHIFT_MASK) != 0;
	/* The key as Waybox will see it: with Shift applied ("exclam" for
	 * Shift+1, as xkb gives it), a letter in lowercase (Waybox folds case). */
	const char *raw = gdk_keyval_name(gdk_keyval_to_lower(keyval));
	std::string name = raw != nullptr ? raw : "";
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

/* ---- The mouse: where, which button, keys held, how ---- */

void on_mouse_changed(ShortcutEdit *e) {
	if (e->updating || !e->mnow)
		return;
	auto pick = [](GtkWidget *dd, const std::vector<std::string> &values) {
		guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
		return i < values.size() ? values[i] : std::string();
	};
	/* The four offered, from the toggles; any rarer one (Caps, Mod2...) kept. */
	std::string was_rest;
	wb::HeldModifiers held = wb::held_of(e->mnow->button, was_rest);
	held.super = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->mods["W-"]));
	held.ctrl = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->mods["C-"]));
	held.alt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->mods["A-"]));
	held.shift = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->mods["S-"]));
	std::string button = pick(e->button, e->buttons);
	e->mnow->context = pick(e->place, e->places);
	e->mnow->button = wb::rc_key(held, button);
	/* The wheel turns, it isn't pressed or dragged: always a click. */
	if (is_wheel(button))
		e->mnow->event = "Click";
	else
		e->mnow->event = pick(e->event, e->events);
	gtk_widget_set_visible(e->event_field, !is_wheel(button));
	changed(e);
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

void build_mouse_fields(ShortcutEdit *e, GtkWidget *box, GtkSizeGroup *labels) {
	const wb::MouseBinding &m = *e->mnow;
	std::string rest;
	wb::HeldModifiers held = wb::held_of(m.button, rest);
	auto on_dd = +[](GObject *, GParamSpec *, gpointer d) {
		on_mouse_changed(static_cast<ShortcutEdit *>(d));
	};
	e->place = dropdown(wb::mouse_contexts(), m.context, e->places);
	g_signal_connect(e->place, "notify::selected", G_CALLBACK(on_dd), e);
	gtk_box_append(GTK_BOX(box), field(labels, _("Where"), e->place));

	GtkWidget *how = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	struct Mod { const char *prefix, *label; bool on; };
	for (const Mod &mod : {Mod{"W-", "Super", held.super}, Mod{"C-", "Ctrl", held.ctrl},
			Mod{"A-", "Alt", held.alt}, Mod{"S-", "Shift", held.shift}}) {
		GtkWidget *t = gtk_toggle_button_new_with_label(mod.label);
		gtk_widget_add_css_class(t, "wb-mod-toggle");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), mod.on);
		gtk_widget_set_tooltip_text(t, _("Held while clicking"));
		g_signal_connect(t, "toggled", G_CALLBACK(+[](GtkToggleButton *, gpointer d) {
			on_mouse_changed(static_cast<ShortcutEdit *>(d));
		}), e);
		e->mods[mod.prefix] = t;
		gtk_box_append(GTK_BOX(how), t);
	}
	gtk_box_append(GTK_BOX(how), dim("+"));
	e->button = dropdown(wb::mouse_buttons(), rest, e->buttons);
	g_signal_connect(e->button, "notify::selected", G_CALLBACK(on_dd), e);
	gtk_box_append(GTK_BOX(how), e->button);
	gtk_box_append(GTK_BOX(box), field(labels, _("Button"), how));

	e->event = dropdown(wb::mouse_events(), m.event, e->events);
	g_signal_connect(e->event, "notify::selected", G_CALLBACK(on_dd), e);
	e->event_field = field(labels, _("When"), e->event);
	gtk_widget_set_visible(e->event_field, !is_wheel(rest));
	gtk_box_append(GTK_BOX(box), e->event_field);
}

/* ---- The action and its options ---- */

wb::BoundAction &first_action(ShortcutEdit *e) {
	auto &a = acts(e);
	if (a.empty())
		a.push_back({"Execute", {}, false});
	return a[0];
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

/* The chosen action's options: only those it has. */
void build_options(ShortcutEdit *e) {
	clear(e->options);
	e->option_widgets.clear();
	/* Read only: a binding with no actions isn't changed by being shown. */
	static const wb::BoundAction none{};
	const wb::BoundAction &a = acts(e).empty() ? none : acts(e)[0];
	const wb::ActionInfo *info = wb::action_info(a.name);
	GtkSizeGroup *labels = GTK_SIZE_GROUP(g_object_get_data(G_OBJECT(e->options), "wb-labels"));
	e->updating = true;
	for (const wb::ActionOption &o : info != nullptr ? info->options
			: std::vector<wb::ActionOption>{}) {
		std::string v = option_value(a, o.name);
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
			g_signal_connect(w, "changed", G_CALLBACK(on_option_text), e);
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
			g_signal_connect(w, "notify::selected", G_CALLBACK(on_option_choice), e);
			break;
		}
		case wb::OptionKind::Bool:
			w = gtk_check_button_new_with_label(o.label.c_str());
			gtk_check_button_set_active(GTK_CHECK_BUTTON(w),
					wb::setting_true(v.empty() ? o.fallback : v));
			g_signal_connect(w, "toggled", G_CALLBACK(on_option_bool), e);
			label = "";
			break;
		}
		g_object_set_data_full(G_OBJECT(w), "wb-option", g_strdup(o.name.c_str()), g_free);
		gtk_box_append(GTK_BOX(e->options), field(labels, label, w));
		e->option_widgets[o.name] = w;
	}
	e->updating = false;
	/* More than one action: the first edited here, the rest kept. */
	const auto &all = acts(e);
	if (all.size() > 1) {
		std::vector<wb::BoundAction> rest(all.begin() + 1, all.end());
		gtk_label_set_text(GTK_LABEL(e->also),
				(std::string(_("Then also: ")) + wb::describe_actions(rest)).c_str());
		gtk_widget_set_visible(e->also, TRUE);
	} else {
		gtk_widget_set_visible(e->also, FALSE);
	}
}

void select_action(ShortcutEdit *e, const std::string &name) {
	for (size_t i = 0; i < e->action_names.size(); ++i)
		if (e->action_names[i] == name)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(e->action), static_cast<guint>(i));
}

void on_action_selected(GObject *, GParamSpec *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->updating || !present(e))
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

/* ---- Opening, removing, putting back ---- */

void set_open(ShortcutEdit *e, bool open) {
	if (!open && e->ui->capturing == e)
		end_capture(e->ui);
	gtk_revealer_set_reveal_child(GTK_REVEALER(e->revealer), open);
	if (open)
		gtk_widget_add_css_class(e->outer, "wb-shortcut-open");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-open");
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
	refresh_clashes(ui);
	refresh_save_state(ui);
}

void drop(ShortcutEdit *e);

void on_remove(GtkButton *, gpointer data) {
	drop(static_cast<ShortcutEdit *>(data));
}

/* The editor's fields as the binding is now. */
void show_in_editor(ShortcutEdit *e) {
	if (!present(e) || e->action == nullptr)
		return;
	e->updating = true;
	if (!acts(e).empty())
		select_action(e, acts(e)[0].name);
	if (e->mouse) {
		std::string rest;
		wb::HeldModifiers held = wb::held_of(e->mnow->button, rest);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["W-"]), held.super);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["C-"]), held.ctrl);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["A-"]), held.alt);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["S-"]), held.shift);
		auto set = [](GtkWidget *dd, const std::vector<std::string> &values, const std::string &v) {
			for (size_t i = 0; i < values.size(); ++i)
				if (values[i] == v)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), static_cast<guint>(i));
		};
		set(e->place, e->places, e->mnow->context);
		set(e->button, e->buttons, rest);
		set(e->event, e->events, e->mnow->event);
		gtk_widget_set_visible(e->event_field, !is_wheel(rest));
	}
	e->updating = false;
	build_options(e);
}

void on_undo(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	set_open(e, false);
	forget_changes(e);
	show_in_editor(e);
	changed(e);
}

GtkWidget *build_editor(ShortcutEdit *e) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "wb-shortcut-editor");
	GtkSizeGroup *labels = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	if (e->mouse) {
		build_mouse_fields(e, box, labels);
	} else {
		/* Keys: click, press. */
		GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		e->keys_button = gtk_button_new();
		gtk_widget_add_css_class(e->keys_button, "wb-keys-button");
		gtk_widget_set_tooltip_text(e->keys_button, _("Click, then press the keys to use"));
		g_signal_connect(e->keys_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			auto *x = static_cast<ShortcutEdit *>(d);
			if (x->ui->capturing == x)
				end_capture(x->ui);
			else
				start_capture(x, false);
		}), e);
		gtk_box_append(GTK_BOX(keys), e->keys_button);
		e->then_button = gtk_button_new_with_label(_("Then another key"));
		gtk_widget_add_css_class(e->then_button, "flat");
		gtk_widget_set_tooltip_text(e->then_button,
				_("A chain: press these keys, let go, then the next one"));
		g_signal_connect(e->then_button, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			start_capture(static_cast<ShortcutEdit *>(d), true);
		}), e);
		gtk_box_append(GTK_BOX(keys), e->then_button);
		e->keys_hint = dim(_("Esc cancels"));
		gtk_box_append(GTK_BOX(keys), e->keys_hint);
		gtk_box_append(GTK_BOX(box), field(labels, _("Keys"), keys));
	}

	/* Action: type to find it. */
	GtkStringList *model = gtk_string_list_new(nullptr);
	guint sel = GTK_INVALID_LIST_POSITION;
	std::string current = acts(e).empty() ? "" : acts(e)[0].name;
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
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(e->action),
			GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
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
	GtkWidget *remove = gtk_button_new_with_label(
			e->mouse ? _("Remove Mouse Binding") : _("Remove Shortcut"));
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_add_css_class(remove, "wb-remove");
	g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), e);
	gtk_box_append(GTK_BOX(foot), remove);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	GtkWidget *done = gtk_button_new_with_label(_("Done"));
	g_signal_connect(done, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		set_open(static_cast<ShortcutEdit *>(d), false);
	}), e);
	gtk_box_append(GTK_BOX(foot), done);
	gtk_box_append(GTK_BOX(box), foot);
	refresh_keys(e);
	return box;
}

/* Gone from the list: removed (until saved), or never saved. */
void drop(ShortcutEdit *e) {
	set_open(e, false);
	if (!was_saved(e)) {
		forget(e);
		return;
	}
	remove_now(e);
	changed(e);
}

/* "Replace It": the other goes, this one runs; "Remove This One": a
 * binding that can't run, gone. */
void on_clash_fix(GtkButton *, gpointer data) {
	auto *e = static_cast<ShortcutEdit *>(data);
	if (e->clash_by == nullptr)
		return;
	if (dirty(e))
		drop(e->clash_by);
	else
		drop(e);
}

/* Where a binding stands once saved: as it is in rc.xml, unless its keys
 * (or place) changed or it's new: then it's written after the others. */
bool moves_to_end(const ShortcutEdit *e) {
	if (!was_saved(e))
		return true;
	if (!present(e))
		return false;
	if (e->mouse)
		return e->mnow->context != e->mwas->context || e->mnow->button != e->mwas->button ||
				e->mnow->event != e->mwas->event;
	return e->now->chain != e->was->chain;
}

void show_clash(ShortcutEdit *e, ShortcutEdit *by, const std::string &text, bool fix,
		const char *kind = "") {
	e->clash_by = by;
	e->clash_kind = kind;
	gtk_label_set_text(GTK_LABEL(e->warn_label), text.c_str());
	gtk_widget_set_visible(e->warn, TRUE);
	gtk_widget_set_visible(e->warn_fix, fix);
	gtk_button_set_label(GTK_BUTTON(e->warn_fix),
			dirty(e) ? _("Replace It") : _("Remove This One"));
	gtk_widget_set_tooltip_text(e->warn_fix, dirty(e)
			? _("Remove the other binding, so this one runs")
			: _("Remove this binding: it can't run"));
}

void refresh_clashes(Ui *ui) {
	for (bool mouse : {false, true}) {
		/* In the order they'll be in once saved. */
		std::vector<ShortcutEdit *> order;
		for (bool at_end : {false, true})
			for (auto &p : ui->shortcut_edits)
				if (p->mouse == mouse && moves_to_end(p.get()) == at_end)
					order.push_back(p.get());
		for (ShortcutEdit *e : order) {
			e->clash_by = nullptr;
			gtk_widget_set_visible(e->warn, FALSE);
		}
		if (mouse) {
			std::vector<wb::MousePlace> places;
			for (ShortcutEdit *e : order)
				places.push_back(e->mnow ? wb::MousePlace{e->mnow->context, e->mnow->button,
						e->mnow->event} : wb::MousePlace{"", "", ""});
			auto clashes = wb::mouse_clashes(places);
			for (size_t i = 0; i < order.size(); ++i) {
				if (order[i]->mnow) {
					const auto &all = wb::mouse_contexts();
					const std::string &ctx = order[i]->mnow->context;
					bool known = std::any_of(all.begin(), all.end(),
							[&ctx](const auto &p) { return p.first == ctx; });
					if (!known) {
						char *t = g_strdup_printf(_("Never runs: Waybox doesn't read clicks on “%s” "
								"(yet). Pick another place."), ctx.c_str());
						show_clash(order[i], nullptr, t, false, "place");
						g_free(t);
						continue;
					}
				}
				if (!clashes[i] || !order[i]->mnow || !order[*clashes[i]]->mnow)
					continue;
				ShortcutEdit *by = order[*clashes[i]];
				std::string text = std::string(_("Never runs: “")) +
						wb::describe_actions(by->mnow->actions) +
						_("” has the same place, button and click.");
				show_clash(order[i], by, text, order[i]->editable && by->editable);
			}
			continue;
		}
		std::vector<std::vector<std::string>> chains;
		for (ShortcutEdit *e : order)
			chains.push_back(e->now ? e->now->chain : std::vector<std::string>{});
		auto clashes = wb::key_clashes(chains, ui->script_keys);
		for (size_t i = 0; i < order.size(); ++i) {
			if (!clashes[i])
				continue;
			const wb::Clash &c = *clashes[i];
			ShortcutEdit *e = order[i];
			std::string keys = chain_caps_text(c.keys);
			char *t;
			if (c.script) {
				t = g_strdup_printf(_("Never runs: %s is bound in %s (line %d), and scripts come first."),
						keys.c_str(), c.script->file.c_str(), c.script->line);
				show_clash(e, nullptr, t, false, "script");
				g_free(t);
				continue;
			}
			ShortcutEdit *by = order[*c.by];
			std::string does = wb::describe_actions(by->now->actions);
			if (c.by_chain)
				t = g_strdup_printf(_("Never runs: %s starts the chain %s (“%s”)."), keys.c_str(),
						chain_caps_text(by->now->chain).c_str(), does.c_str());
			else if (c.keys.size() < e->now->chain.size())
				t = g_strdup_printf(_("Never runs: %s is taken by “%s”, so the chain never starts."),
						keys.c_str(), does.c_str());
			else
				t = g_strdup_printf(_("Never runs: %s is taken by “%s”."), keys.c_str(), does.c_str());
			show_clash(e, by, t, e->editable && by->editable);
			g_free(t);
		}
	}
}

/* A binding's row: a button that opens its editor underneath, or for one
 * only rc.xml edits, a quiet line saying so. */
ShortcutEdit *add_row(Ui *ui, std::shared_ptr<ShortcutEdit> e, GtkSizeGroup *column) {
	e->ui = ui;
	e->editable = !any_advanced(shown_acts(e.get()));
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
		g_signal_connect(e->head, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			auto *x = static_cast<ShortcutEdit *>(d);
			if (present(x))
				set_open(x, !gtk_revealer_get_reveal_child(GTK_REVEALER(x->revealer)));
		}), e.get());
	} else {
		e->head = line;
		gtk_widget_add_css_class(e->head, "wb-shortcut-row");
		gtk_widget_add_css_class(e->head, "wb-shortcut-static");
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

	e->warn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(e->warn, "wb-shortcut-warn");
	GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
	gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(e->warn), icon);
	e->warn_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->warn_label), 0);
	gtk_label_set_wrap(GTK_LABEL(e->warn_label), TRUE);
	gtk_widget_set_hexpand(e->warn_label, TRUE);
	gtk_box_append(GTK_BOX(e->warn), e->warn_label);
	e->warn_fix = gtk_button_new_with_label("");
	gtk_widget_add_css_class(e->warn_fix, "flat");
	gtk_widget_set_valign(e->warn_fix, GTK_ALIGN_CENTER);
	g_signal_connect(e->warn_fix, "clicked", G_CALLBACK(on_clash_fix), e.get());
	gtk_box_append(GTK_BOX(e->warn), e->warn_fix);
	gtk_widget_set_visible(e->warn, FALSE);
	gtk_box_append(GTK_BOX(e->outer), e->warn);

	e->revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(e->revealer),
			GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(e->revealer), 150);
	if (e->editable && present(e.get()))
		gtk_revealer_set_child(GTK_REVEALER(e->revealer), build_editor(e.get()));
	gtk_box_append(GTK_BOX(e->outer), e->revealer);
	refresh_head(e.get());
	ui->shortcut_edits.push_back(e);
	return e.get();
}

ShortcutEdit *key_row(Ui *ui, std::optional<wb::KeyBinding> was, std::optional<wb::KeyBinding> now) {
	auto e = std::make_shared<ShortcutEdit>();
	e->was = std::move(was);
	e->now = std::move(now);
	return add_row(ui, e, ui->shortcut_column);
}

ShortcutEdit *mouse_row(Ui *ui, std::optional<wb::MouseBinding> was,
		std::optional<wb::MouseBinding> now) {
	auto e = std::make_shared<ShortcutEdit>();
	e->mouse = true;
	e->mwas = std::move(was);
	e->mnow = std::move(now);
	return add_row(ui, e, ui->shortcut_mouse_column);
}

/* A new row, open, just above its list's Add button. */
ShortcutEdit *add_new(Ui *ui, bool mouse) {
	ShortcutEdit *e;
	GtkWidget *list = mouse ? ui->shortcut_mouse : ui->shortcut_keys;
	GtkWidget *add = mouse ? ui->shortcut_mouse_add : ui->shortcut_add;
	if (mouse)
		e = mouse_row(ui, std::nullopt,
				wb::MouseBinding{"Root", "Middle", "Click", {{"Execute", {}, false}}});
	else
		e = key_row(ui, std::nullopt, wb::KeyBinding{{}, {{"Execute", {}, false}}});
	/* At the end of the list, just above its foot (Add, Reset). */
	gtk_box_insert_child_after(GTK_BOX(list), e->outer,
			gtk_widget_get_prev_sibling(gtk_widget_get_parent(add)));
	set_open(e, true);
	if (!mouse)
		start_capture(e, false);
	refresh_save_state(ui);
	return e;
}

/* Rows under a heading per category, in the catalog's order. */
void fill(GtkWidget *box, const std::map<std::string, std::vector<ShortcutEdit *>> &groups) {
	for (const std::string &cat : category_order()) {
		auto it = groups.find(cat);
		if (it == groups.end())
			continue;
		GtkWidget *h = gtk_label_new(cat.c_str());
		gtk_label_set_xalign(GTK_LABEL(h), 0);
		gtk_widget_add_css_class(h, "wb-shortcut-group");
		gtk_box_append(GTK_BOX(box), h);
		for (ShortcutEdit *e : it->second) {
			e->heading = h;
			gtk_box_append(GTK_BOX(box), e->outer);
		}
	}
}

GtkWidget *add_button(Ui *ui, const char *label, bool mouse) {
	GtkWidget *b = gtk_button_new();
	GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(inside), gtk_image_new_from_icon_name("list-add-symbolic"));
	gtk_box_append(GTK_BOX(inside), gtk_label_new(label));
	gtk_button_set_child(GTK_BUTTON(b), inside);
	gtk_widget_add_css_class(b, "flat");
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	gtk_widget_set_margin_top(b, 8);
	if (mouse)
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			add_new(static_cast<Ui *>(d), true);
		}), ui);
	else
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			add_new(static_cast<Ui *>(d), false);
		}), ui);
	return b;
}

/* ---- Search ---- */

std::string fold(const std::string &s) {
	gchar *f = g_utf8_casefold(s.c_str(), -1);
	std::string out = f;
	g_free(f);
	return out;
}

/* Every word of the query somewhere in what the row says; a one-letter
 * word only as a whole key ("super e" finds Super+E, not every "e"). */
bool row_matches(const ShortcutEdit *e, const std::vector<std::string> &words) {
	std::vector<std::string> caps_seen;
	std::string hay;
	if (e->mouse) {
		const wb::MouseBinding &m = e->mnow ? *e->mnow : *e->mwas;
		hay = mouse_text(m) + " " + m.context + " " + m.button;
		for (const std::string &c : wb::button_caps(m.button))
			caps_seen.push_back(fold(c));
	} else {
		const auto &chain = e->now ? e->now->chain : e->was->chain;
		hay = chain_caps_text(chain) + " " + chain_text(chain);
		for (const std::string &k : chain)
			for (const std::string &c : wb::key_caps(k)) {
				hay += " " + c;
				caps_seen.push_back(fold(c));
			}
	}
	const auto &a = shown_acts(e);
	hay += " " + wb::describe_actions(a) + " " + category_of(a);
	for (const wb::BoundAction &b : a)
		if (const wb::ActionInfo *info = wb::action_info(b.name))
			hay += " " + info->label + " " + info->name;
	hay = fold(hay);
	/* The words of what it does, for one-letter words ("desktop 2"). */
	std::vector<std::string> said;
	{
		std::string w;
		for (char c : fold(wb::describe_actions(shown_acts(e))) + " ") {
			if (std::isalnum(static_cast<unsigned char>(c))) {
				w += c;
			} else if (!w.empty()) {
				said.push_back(w);
				w.clear();
			}
		}
	}
	for (const std::string &w : words) {
		if (g_utf8_strlen(w.c_str(), -1) == 1) {
			if (std::find(caps_seen.begin(), caps_seen.end(), w) == caps_seen.end() &&
					std::find(said.begin(), said.end(), w) == said.end())
				return false;
		} else if (hay.find(w) == std::string::npos) {
			return false;
		}
	}
	return true;
}

std::vector<std::string> words_of(const std::string &q) {
	std::vector<std::string> out;
	std::string f = fold(q), w;
	for (char c : f + " ") {
		if (c == ' ' || c == '+') {
			if (!w.empty())
				out.push_back(w);
			w.clear();
		} else {
			w += c;
		}
	}
	return out;
}

/* Actions the query names that no key runs yet: offered to bind. */
void suggest(Ui *ui, const std::vector<std::string> &words) {
	clear(ui->shortcut_suggest);
	if (words.empty()) {
		gtk_widget_set_visible(ui->shortcut_suggest, FALSE);
		return;
	}
	int n = 0;
	for (const wb::ActionInfo &a : wb::action_catalog()) {
		std::string hay = fold(a.label + " " + a.name + " " + a.category);
		bool match = std::all_of(words.begin(), words.end(),
				[&hay](const std::string &w) { return hay.find(w) != std::string::npos; });
		if (!match)
			continue;
		bool bound = false;
		for (auto &e : ui->shortcut_edits)
			if (!e->mouse && e->now && !e->now->actions.empty() &&
					wb::action_info(e->now->actions[0].name) == &a)
				bound = true;
		if (bound || n >= 3)
			continue;
		++n;
		std::string label = std::string(_("Add a shortcut for ")) + a.label;
		GtkWidget *b = gtk_button_new();
		GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_append(GTK_BOX(inside), gtk_image_new_from_icon_name("list-add-symbolic"));
		gtk_box_append(GTK_BOX(inside), gtk_label_new(label.c_str()));
		gtk_button_set_child(GTK_BUTTON(b), inside);
		gtk_widget_add_css_class(b, "flat");
		gtk_widget_set_halign(b, GTK_ALIGN_START);
		g_object_set_data_full(G_OBJECT(b), "wb-action", g_strdup(a.name.c_str()), g_free);
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
			auto *ui = static_cast<Ui *>(d);
			std::string name = static_cast<const char *>(
					g_object_get_data(G_OBJECT(btn), "wb-action"));
			ShortcutEdit *e = add_new(ui, false);
			select_action(e, name);
		}), ui);
		gtk_box_append(GTK_BOX(ui->shortcut_suggest), b);
	}
	gtk_widget_set_visible(ui->shortcut_suggest, n > 0);
}

ShortcutEdit *find_edit(Ui *ui, const std::string &id) {
	ShortcutEdit *removed = nullptr;
	for (auto &e : ui->shortcut_edits)
		if (id_of(e.get()) == id) {
			if (present(e.get()))
				return e.get();
			if (removed == nullptr)
				removed = e.get();
		}
	return removed;
}

std::vector<std::string> split_words(const std::string &s) {
	std::vector<std::string> out;
	std::string w;
	for (char c : s + " ") {
		if (c == ' ') {
			if (!w.empty())
				out.push_back(w);
			w.clear();
		} else {
			w += c;
		}
	}
	return out;
}

/* The keys the scripts Waybox runs bind. */
std::vector<wb::ScriptKey> read_script_keys(const wb::RcDocument *doc) {
	std::vector<wb::ScriptKey> out;
	for (const auto &[name, text] : script_sources(doc))
		for (wb::ScriptKey &k : wb::script_keys(text, name))
			out.push_back(std::move(k));
	return out;
}

/* Reset to Defaults: the lists made Waybox's own, as changes not saved
 * yet (each undoable; nothing written until Save & Apply). Bindings with
 * If and the like stay, as only rc.xml edits those. */
void reset_to_defaults(Ui *ui, bool mouse) {
	end_capture(ui);
	std::string err;
	wb::LoadError le;
	auto defaults = wb::RcDocument::load_file(system_rc_path(), &le);
	if (!defaults) {
		set_status(ui, _("Waybox's default rc.xml couldn't be read."));
		return;
	}
	std::vector<ShortcutEdit *> rows;
	for (auto &p : ui->shortcut_edits)
		if (p->mouse == mouse)
			rows.push_back(p.get());
	if (mouse) {
		std::vector<wb::MouseBinding> want = defaults->mousebindings();
		std::vector<bool> used(want.size(), false);
		for (ShortcutEdit *e : rows) {
			if (!e->editable)
				continue;
			const wb::MouseBinding &m = e->mwas ? *e->mwas : *e->mnow;
			bool kept = false;
			for (size_t i = 0; i < want.size() && !kept; ++i)
				if (!used[i] && want[i].context == m.context && want[i].event == m.event &&
						wb::canonical_key(want[i].button) == wb::canonical_key(m.button)) {
					used[i] = true;
					/* A default only rc.xml can write (If...): this one left. */
					if (!any_advanced(want[i].actions))
						e->mnow = want[i];
					kept = true;
				}
			if (!kept)
				e->mnow.reset();
		}
		for (size_t i = 0; i < want.size(); ++i)
			if (!used[i] && !any_advanced(want[i].actions))
				mouse_row(ui, std::nullopt, want[i]);
	} else {
		std::vector<wb::KeyBinding> want = defaults->keybindings();
		std::vector<bool> used(want.size(), false);
		auto same = [](const std::vector<std::string> &a, const std::vector<std::string> &b) {
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (wb::canonical_key(a[i]) != wb::canonical_key(b[i]))
					return false;
			return true;
		};
		for (ShortcutEdit *e : rows) {
			if (!e->editable)
				continue;
			const wb::KeyBinding &k = e->was ? *e->was : *e->now;
			bool kept = false;
			for (size_t i = 0; i < want.size() && !kept; ++i)
				if (!used[i] && same(want[i].chain, k.chain)) {
					used[i] = true;
					if (!any_advanced(want[i].actions))
						e->now = want[i];
					kept = true;
				}
			if (!kept)
				e->now.reset();
		}
		for (size_t i = 0; i < want.size(); ++i)
			if (!used[i] && !any_advanced(want[i].actions))
				key_row(ui, std::nullopt, want[i]);
	}
	/* New rows never saved and now not wanted: gone. */
	std::vector<ShortcutEdit *> gone;
	for (auto &p : ui->shortcut_edits)
		if (p->mouse == mouse && !was_saved(p.get()) && !present(p.get()))
			gone.push_back(p.get());
	for (ShortcutEdit *e : gone)
		forget(e);
	/* The rows added: above the Add button. */
	GtkWidget *list = mouse ? ui->shortcut_mouse : ui->shortcut_keys;
	GtkWidget *add = mouse ? ui->shortcut_mouse_add : ui->shortcut_add;
	for (auto &p : ui->shortcut_edits)
		if (p->mouse == mouse && gtk_widget_get_parent(p->outer) == nullptr)
			gtk_box_insert_child_after(GTK_BOX(list), p->outer,
					gtk_widget_get_prev_sibling(gtk_widget_get_parent(add)));
	for (auto &p : ui->shortcut_edits)
		if (p->mouse == mouse) {
			show_in_editor(p.get());
			refresh_head(p.get());
		}
	refresh_clashes(ui);
	refresh_save_state(ui);
}

/* The list's foot: Add, and Reset to Defaults. */
GtkWidget *list_foot(Ui *ui, GtkWidget *add, bool mouse) {
	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(foot, 8);
	gtk_widget_set_margin_top(add, 0);
	gtk_box_append(GTK_BOX(foot), add);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	GtkWidget *reset = gtk_button_new_with_label(_("Reset to Defaults"));
	gtk_widget_add_css_class(reset, "flat");
	gtk_widget_set_tooltip_text(reset, mouse
			? _("Waybox's own mouse bindings, as changes you can undo or save")
			: _("Waybox's own shortcuts, as changes you can undo or save"));
	if (mouse)
		g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			reset_to_defaults(static_cast<Ui *>(d), true);
		}), ui);
	else
		g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
			reset_to_defaults(static_cast<Ui *>(d), false);
		}), ui);
	gtk_box_append(GTK_BOX(foot), reset);
	return foot;
}

}  // namespace

/* The scripts Waybox runs, as Waybox finds them: init.lua, then rc.xml's
 * <scripts> (relative to the config folder, or ~/...); each its file's
 * name and text. Those that can't be read are left out. */
std::vector<std::pair<std::string, std::string>> script_sources(const wb::RcDocument *doc) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	std::string config = (xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.config") +
			"/waybox";
	std::vector<std::string> paths = {config + "/init.lua"};
	std::string xml = doc->snapshot({"waybox", "scripts"});
	/* A commented-out <script> isn't one. */
	for (size_t c; (c = xml.find("<!--")) != std::string::npos;) {
		size_t end = xml.find("-->", c);
		xml.erase(c, end == std::string::npos ? std::string::npos : end + 3 - c);
	}
	for (size_t at = 0; (at = xml.find("<script", at)) != std::string::npos;) {
		size_t open = xml.find('>', at), close = xml.find("</script>", at);
		if (open == std::string::npos || close == std::string::npos || open > close ||
				xml.compare(at, 8, "<scripts") == 0) {
			at += 7;
			continue;
		}
		std::string p = xml.substr(open + 1, close - open - 1);
		while (!p.empty() && isspace(static_cast<unsigned char>(p.back())))
			p.pop_back();
		while (!p.empty() && isspace(static_cast<unsigned char>(p.front())))
			p.erase(0, 1);
		if (p.rfind("~/", 0) == 0 && home)
			p = std::string(home) + p.substr(1);
		else if (!p.empty() && p[0] != '/')
			p = config + "/" + p;
		if (!p.empty())
			paths.push_back(p);
		at = close + 9;
	}
	std::vector<std::pair<std::string, std::string>> out;
	for (const std::string &p : paths) {
		std::ifstream in(p);
		if (!in)
			continue;
		std::stringstream text;
		text << in.rdbuf();
		out.emplace_back(p.substr(p.rfind('/') + 1), text.str());
	}
	return out;
}

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
	ui->shortcut_edits.clear();
	ui->script_keys = read_script_keys(doc);
	if (ui->shortcut_keys != nullptr) {
		clear(ui->shortcut_keys);
		if (ui->shortcut_column != nullptr)
			g_object_unref(ui->shortcut_column);
		ui->shortcut_column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
		std::map<std::string, std::vector<ShortcutEdit *>> groups;
		for (const wb::KeyBinding &k : doc->keybindings())
			groups[category_of(k.actions)].push_back(key_row(ui, k, k));
		ui->shortcut_suggest = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_add_css_class(ui->shortcut_suggest, "wb-shortcut-suggest");
		gtk_widget_set_visible(ui->shortcut_suggest, FALSE);
		gtk_box_append(GTK_BOX(ui->shortcut_keys), ui->shortcut_suggest);
		fill(ui->shortcut_keys, groups);
		ui->shortcut_add = add_button(ui, _("Add a Shortcut"), false);
		gtk_box_append(GTK_BOX(ui->shortcut_keys), list_foot(ui, ui->shortcut_add, false));
	}
	if (ui->shortcut_mouse != nullptr) {
		clear(ui->shortcut_mouse);
		if (ui->shortcut_mouse_column != nullptr)
			g_object_unref(ui->shortcut_mouse_column);
		ui->shortcut_mouse_column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
		std::map<std::string, std::vector<ShortcutEdit *>> groups;
		for (const wb::MouseBinding &m : doc->mousebindings())
			groups[category_of(m.actions)].push_back(mouse_row(ui, m, m));
		fill(ui->shortcut_mouse, groups);
		ui->shortcut_mouse_add = add_button(ui, _("Add a Mouse Binding"), true);
		gtk_box_append(GTK_BOX(ui->shortcut_mouse), list_foot(ui, ui->shortcut_mouse_add, true));
	}
	refresh_clashes(ui);
}

bool shortcuts_dirty(Ui *ui, const std::string &widget) {
	bool mouse = widget == "mousebindings";
	for (auto &e : ui->shortcut_edits)
		if (e->mouse == mouse && dirty(e.get()) && !incomplete(e.get()))
			return true;
	return false;
}

bool shortcuts_problem(Ui *ui, const std::string &widget) {
	bool mouse = widget == "mousebindings";
	for (auto &e : ui->shortcut_edits)
		if (e->mouse == mouse && gtk_widget_get_visible(e->warn))
			return true;
	return false;
}

bool shortcuts_filter(Ui *ui, const std::string &widget, const std::string &q, bool setting_hit) {
	bool mouse = widget == "mousebindings";
	/* Search's @-filters on rows: @modified the unsaved ones, @problems
	 * the ones that never run; @id: is the setting's (setting_hit). */
	bool want_modified = false, want_problems = false, any_at = false;
	std::vector<std::string> words;
	for (const std::string &w : words_of(q)) {
		if (w == "@modified")
			want_modified = true;
		else if (w == "@problems")
			want_problems = true;
		if (!w.empty() && w[0] == '@')
			any_at = true;
		else
			words.push_back(w);
	}
	auto passes_at = [&](ShortcutEdit *e) {
		return (!want_modified || (dirty(e) && !incomplete(e))) &&
				(!want_problems || gtk_widget_get_visible(e->warn));
	};
	/* Rows matching the words come first; all rows (that pass the
	 * @-filters) when none does but the setting itself is what's searched
	 * for ("hotkey"). Nothing when neither. */
	bool rows_match = false;
	for (auto &e : ui->shortcut_edits)
		rows_match = rows_match || (e->mouse == mouse && was_saved(e.get()) && passes_at(e.get()) &&
				!words.empty() && row_matches(e.get(), words));
	bool by_words = !words.empty() && rows_match;
	bool all = words.empty() ? setting_hit : (!rows_match && setting_hit);
	bool any = false;
	std::map<GtkWidget *, bool> headings;
	for (auto &e : ui->shortcut_edits) {
		if (e->mouse != mouse)
			continue;
		bool show;
		if (q.empty())
			show = true;
		else if (!was_saved(e.get()))
			show = !want_problems;  /* new rows stay in sight while being set up */
		else
			show = passes_at(e.get()) && ((by_words && row_matches(e.get(), words)) || all);
		gtk_widget_set_visible(e->outer, show);
		any = any || show;
		if (e->heading != nullptr)
			headings[e->heading] = headings[e->heading] || show;
	}
	for (auto &[h, show] : headings)
		gtk_widget_set_visible(h, show);
	bool suggestions = false;
	if (!mouse && ui->shortcut_suggest != nullptr) {
		suggest(ui, any_at || all ? std::vector<std::string>{} : words);
		suggestions = gtk_widget_get_visible(ui->shortcut_suggest);
	}
	return q.empty() || (any && (setting_hit || by_words)) || suggestions;
}

void apply_shortcuts(Ui *ui, wb::RcDocument &doc) {
	end_capture(ui);
	/* Keys and places given up first (removed or moved), so a binding
	 * moving onto ones another just left finds them free. */
	for (auto &e : ui->shortcut_edits) {
		if (!dirty(e.get()) || !was_saved(e.get()))
			continue;
		if (e->mouse) {
			const auto &w = *e->mwas;
			if (!e->mnow || e->mnow->context != w.context || e->mnow->button != w.button ||
					e->mnow->event != w.event)
				doc.set_mousebinding(e->mwas, std::nullopt);
		} else if (!e->now || e->now->chain != e->was->chain) {
			/* Told apart by what it ran, should two have the same keys. */
			doc.set_keybinding(e->was->chain, std::nullopt, &e->was->actions);
		}
	}
	for (auto &e : ui->shortcut_edits) {
		if (!dirty(e.get()) || !present(e.get()) || incomplete(e.get()))
			continue;
		if (e->mouse) {
			const auto &n = *e->mnow;
			bool same_place = e->mwas && e->mwas->context == n.context &&
					e->mwas->button == n.button && e->mwas->event == n.event;
			doc.set_mousebinding(same_place ? e->mwas : std::nullopt, e->mnow);
		} else {
			bool same_keys = e->was && e->was->chain == e->now->chain;
			if (same_keys)
				doc.set_keybinding(e->was->chain, e->now, &e->was->actions);
			else
				doc.set_keybinding({}, e->now);
		}
	}
}

void shortcut_changes(Ui *ui, const std::string &widget, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out) {
	bool mouse = widget == "mousebindings";
	auto key_text = [](const std::optional<wb::KeyBinding> &b) {
		return b ? chain_caps_text(b->chain) + "  " + wb::describe_actions(b->actions)
				: std::string(_("none"));
	};
	auto mouse_line = [](const std::optional<wb::MouseBinding> &b) {
		return b ? mouse_text(*b) + "  " + wb::describe_actions(b->actions)
				: std::string(_("none"));
	};
	for (auto &e : ui->shortcut_edits) {
		if (e->mouse != mouse || !dirty(e.get()) || incomplete(e.get()))
			continue;  /* the same, or new and not given keys yet */
		if (mouse)
			out.push_back({{where, mouse_line(e->mwas), mouse_line(e->mnow)}, target});
		else
			out.push_back({{where, key_text(e->was), key_text(e->now)}, target});
	}
}

void shortcut_dump(Ui *ui) {
	fprintf(stderr, "shortcut-dump:\n");  /* where each dump starts */
	for (bool mouse : {false, true})
		for (auto &e : ui->shortcut_edits) {
			if (e->mouse != mouse)
				continue;
			const auto &a = shown_acts(e.get());
			std::string state = !present(e.get()) ? "|removed" : dirty(e.get()) ? "|changed" : "";
			if (gtk_widget_get_visible(e->warn))
				state += "|never:" + (e->clash_by ? id_of(e->clash_by) : std::string(e->clash_kind));
			fprintf(stderr, "shortcut:%s|%s|%s|%s%s\n", mouse ? "mouse" : "key",
					id_of(e.get()).c_str(), wb::describe_actions(a).c_str(),
					category_of(a).c_str(), state.c_str());
		}
}

/* Test steps, through the same widgets a person uses (ID: a key chain
 * "C-a x", or a mouse binding's "Context Button Event"):
 *   shortcut-open:ID   shortcut-capture:ID   shortcut-remove:ID   shortcut-undo:ID
 *   shortcut-keys:ID=NEWCHAIN (as if pressed)
 *   shortcut-place:ID=Context  shortcut-button:ID=Button  shortcut-event:ID=Event
 *   shortcut-mod:ID=W-|C-|A-|S- (toggled)
 *   shortcut-action:ID=Name    shortcut-option:ID=name=value
 *   shortcut-add:CHAIN=Name    shortcut-addmouse:Context Button Event=Name
 *   shortcut-visible (the rows search shows)   shortcut-suggest:Name (click it)
 */
void shortcut_test_step(Ui *ui, const std::string &step) {
	size_t colon = step.find(':');
	std::string verb = step.substr(0, colon);
	std::string arg = colon == std::string::npos ? "" : step.substr(colon + 1);
	size_t eq = arg.find('=');
	std::string id = arg.substr(0, eq), rest = eq == std::string::npos ? "" : arg.substr(eq + 1);
	auto pick = [](GtkWidget *dd, const std::vector<std::string> &values, const std::string &v) {
		for (size_t i = 0; i < values.size(); ++i)
			if (values[i] == v)
				gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), static_cast<guint>(i));
	};
	if (verb == "shortcut-visible") {
		std::string out, sugg;
		for (auto &e : ui->shortcut_edits)
			if (gtk_widget_get_visible(e->outer) && gtk_widget_get_mapped(e->outer))
				out += (out.empty() ? "" : ",") + id_of(e.get());
		if (ui->shortcut_suggest != nullptr && gtk_widget_get_visible(ui->shortcut_suggest))
			for (GtkWidget *b = gtk_widget_get_first_child(ui->shortcut_suggest); b != nullptr;
					b = gtk_widget_get_next_sibling(b))
				sugg += (sugg.empty() ? "" : ",") +
						std::string(static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-action")));
		fprintf(stderr, "shortcut-visible:%s|suggest:%s\n", out.c_str(), sugg.c_str());
		return;
	}
	if (verb == "shortcut-order") {
		/* The key list as on screen: rows by id, "#" a heading, "foot". */
		std::string out;
		for (GtkWidget *c = gtk_widget_get_first_child(ui->shortcut_keys); c != nullptr;
				c = gtk_widget_get_next_sibling(c)) {
			std::string item;
			for (auto &e : ui->shortcut_edits)
				if (e->outer == c)
					item = id_of(e.get());
			if (item.empty())
				item = c == gtk_widget_get_parent(ui->shortcut_add) ? "foot"
						: c == ui->shortcut_suggest ? "suggest" : "#";
			out += (out.empty() ? "" : ",") + item;
		}
		fprintf(stderr, "shortcut-order:%s\n", out.c_str());
		return;
	}
	if (verb == "shortcut-reset") {
		reset_to_defaults(ui, arg == "mouse");
		return;
	}
	if (verb == "shortcut-fix") {
		if (ShortcutEdit *e = find_edit(ui, arg))
			if (gtk_widget_get_visible(e->warn_fix))
				on_clash_fix(nullptr, e);
		return;
	}
	if (verb == "shortcut-suggest") {
		for (GtkWidget *b = gtk_widget_get_first_child(ui->shortcut_suggest); b != nullptr;
				b = gtk_widget_get_next_sibling(b))
			if (arg == static_cast<const char *>(g_object_get_data(G_OBJECT(b), "wb-action"))) {
				g_signal_emit_by_name(b, "clicked");
				break;
			}
		end_capture(ui);
		return;
	}
	if (verb == "shortcut-add" || verb == "shortcut-addmouse") {
		bool mouse = verb == "shortcut-addmouse";
		ShortcutEdit *e = add_new(ui, mouse);
		end_capture(ui);
		if (mouse) {
			std::vector<std::string> p = split_words(id);
			if (p.size() == 3) {
				std::string r;
				wb::HeldModifiers held = wb::held_of(p[1], r);
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["W-"]), held.super);
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["C-"]), held.ctrl);
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["A-"]), held.alt);
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->mods["S-"]), held.shift);
				pick(e->place, e->places, p[0]);
				pick(e->button, e->buttons, r);
				pick(e->event, e->events, p[2]);
			}
		} else {
			e->now->chain = split_words(id);
			changed(e);
		}
		select_action(e, rest);
		return;
	}
	ShortcutEdit *e = find_edit(ui, id);
	if (e == nullptr) {
		fprintf(stderr, "shortcut-step: no binding %s\n", id.c_str());
		return;
	}
	if (verb == "shortcut-open") {
		set_open(e, true);
	} else if (verb == "shortcut-capture") {
		set_open(e, true);
		start_capture(e, false);
	} else if (verb == "shortcut-keys" && e->now) {
		e->now->chain = split_words(rest);
		changed(e);
	} else if (verb == "shortcut-place" && e->mouse) {
		pick(e->place, e->places, rest);
	} else if (verb == "shortcut-button" && e->mouse) {
		pick(e->button, e->buttons, rest);
	} else if (verb == "shortcut-event" && e->mouse) {
		pick(e->event, e->events, rest);
	} else if (verb == "shortcut-mod" && e->mouse && e->mods.count(rest)) {
		GtkToggleButton *t = GTK_TOGGLE_BUTTON(e->mods[rest]);
		gtk_toggle_button_set_active(t, !gtk_toggle_button_get_active(t));
	} else if (verb == "shortcut-action") {
		select_action(e, rest);
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
