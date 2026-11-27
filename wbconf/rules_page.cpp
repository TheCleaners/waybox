/*
 * Window rules (#237): rc.xml's <applications>, as a list in the order
 * they're tried (the first that matches a window wins): what each matches
 * (the program, a title, a tag) and what it does, in words.
 *
 * A rule is edited where it stands: its row opens underneath with what it
 * matches (simple patterns or, opt-in, regular expressions) and each
 * setting, "As usual" leaving that to Waybox. Rules are added, removed
 * (undoable until saved) and moved earlier or later. Each row keeps the
 * rule as saved and as it is now; Save & Apply writes what changed, then
 * puts them in order. What Waybox doesn't read is kept as written.
 */
#include "wbconf_ui.hpp"

#include <algorithm>
#include <map>
#include <string_view>

#include "waybox/applications.hpp"
#include "waybox/binding_clash.hpp"

namespace wbconf {

struct RuleEdit {
	Ui *ui = nullptr;
	std::optional<wb::AppRule> was, now;
	std::optional<size_t> orig;   /* its index in rc.xml as loaded */
	bool position_kept = false;
	std::vector<std::string> kept;
	bool program_is_name = false; /* the program field edits name=, not class= */
	GtkWidget *outer = nullptr, *head = nullptr, *number = nullptr, *match_cell = nullptr;
	GtkWidget *does = nullptr, *kept_label = nullptr, *error_label = nullptr;
	GtkWidget *undo = nullptr, *revealer = nullptr;
	/* The editor. */
	GtkWidget *program = nullptr, *title = nullptr, *tag = nullptr, *kind = nullptr;
	GtkWidget *hint = nullptr, *editor_error = nullptr;
	GtkWidget *desktop = nullptr, *width = nullptr, *height = nullptr, *x = nullptr, *y = nullptr;
	GtkWidget *layer = nullptr, *monitor = nullptr;
	std::vector<std::string> monitor_values;
	std::map<std::string, GtkWidget *> tri;  /* maximized, fullscreen, ... */
	std::vector<std::string> desktop_values;
	GtkWidget *earlier = nullptr, *later = nullptr;
	/* Pick a Window, and the open windows the rule catches. */
	std::weak_ptr<RuleEdit> self;
	GtkWidget *pick = nullptr, *use_title = nullptr, *matches = nullptr;
	std::string picked_title;
	bool picking = false;
	bool updating = false;
};

namespace {

GtkWidget *dim(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "dim-label");
	return l;
}

GtkWidget *chip(const std::string &text) {
	GtkWidget *l = gtk_label_new(text.c_str());
	gtk_widget_add_css_class(l, "wb-keycap");
	gtk_widget_add_css_class(l, "wb-rule-pattern");
	return l;
}

void clear(GtkWidget *box) {
	while (GtkWidget *c = gtk_widget_get_first_child(box))
		gtk_box_remove(GTK_BOX(box), c);
}

std::string join(const std::vector<std::string> &parts, const char *sep) {
	std::string out;
	for (const std::string &p : parts)
		out += (out.empty() ? "" : sep) + p;
	return out;
}

/* "foot  titled  *scratch*": what a rule matches. */
GtkWidget *match_widget(const wb::AppRule &r) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	bool any = false;
	/* Openbox's class and name both match the app id here. */
	std::string app = !r.class_pattern.empty() ? r.class_pattern : r.name_pattern;
	if (!app.empty()) {
		gtk_box_append(GTK_BOX(box), chip(app));
		any = true;
	}
	if (!r.class_pattern.empty() && !r.name_pattern.empty() && r.name_pattern != r.class_pattern) {
		gtk_box_append(GTK_BOX(box), dim(_("and")));
		gtk_box_append(GTK_BOX(box), chip(r.name_pattern));
	}
	if (!r.title_pattern.empty()) {
		gtk_box_append(GTK_BOX(box), dim(any ? _("titled") : _("Titled")));
		gtk_box_append(GTK_BOX(box), chip(r.title_pattern));
		any = true;
	}
	if (!r.tag_pattern.empty()) {
		gtk_box_append(GTK_BOX(box), dim(any ? _("tagged") : _("Tagged")));
		gtk_box_append(GTK_BOX(box), chip(r.tag_pattern));
		any = true;
	}
	if (!any) {
		GtkWidget *l = gtk_label_new(_("Every window"));
		gtk_widget_add_css_class(l, "wb-rule-every");
		gtk_box_append(GTK_BOX(box), l);
	}
	if (r.patterns != wb::AppRule::Patterns::Glob) {
		GtkWidget *l = dim(r.patterns == wb::AppRule::Patterns::IRegex ? _("regex, any case")
				: _("regex"));
		gtk_widget_add_css_class(l, "wb-rule-kind");
		gtk_widget_set_tooltip_text(l, _("Its patterns are regular expressions, as egrep reads "
				"them: anywhere in the text unless anchored with ^ and $"));
		gtk_box_append(GTK_BOX(box), l);
	}
	return box;
}

/* The same, as text: "foot titled *scratch*". */
std::string match_text(const wb::AppRule &r) {
	std::vector<std::string> parts;
	std::string app = !r.class_pattern.empty() ? r.class_pattern : r.name_pattern;
	if (!app.empty())
		parts.push_back(app);
	if (!r.title_pattern.empty())
		parts.push_back(std::string(_("titled")) + " " + r.title_pattern);
	if (!r.tag_pattern.empty())
		parts.push_back(std::string(_("tagged")) + " " + r.tag_pattern);
	return parts.empty() ? std::string(_("Every window")) : join(parts, " ");
}

std::string rule_text(const std::optional<wb::AppRule> &r) {
	if (!r)
		return _("none");
	std::vector<std::string> effects = wb::app_rule_effects(*r);
	return match_text(*r) + ": " + (effects.empty() ? std::string(_("nothing set"))
			: join(effects, ", "));
}

const wb::AppRule &shown(const RuleEdit *e) {
	return e->now ? *e->now : *e->was;
}

bool dirty(const RuleEdit *e) {
	return e->now != e->was;
}

/* New, and neither matching nor setting anything yet: not a change. */
bool incomplete(const RuleEdit *e) {
	return !e->orig && e->now && *e->now == wb::AppRule{};
}

int desktop_count(Ui *ui) {
	for (auto &r : ui->rows)
		if (r->def->id == "desktops.number") {
			int n = atoi(r->value.value_or(r->def->def).c_str());
			return n >= 1 ? n : 4;
		}
	return 4;
}

void refresh_numbers(Ui *ui);

/* ---- The open windows (Waybox's get_windows), for live matches ---- */

struct OpenWindow {
	std::string app_id, title, tag;
};

/* Waybox's windows, asked for at most every two seconds; nullopt when
 * Waybox can't be asked (not running, its socket off). */
const std::optional<std::vector<OpenWindow>> &open_windows() {
	static std::optional<std::vector<OpenWindow>> cached;
	static gint64 at = 0;
	gint64 now = g_get_monotonic_time();
	if (at != 0 && now - at < 2 * G_USEC_PER_SEC)
		return cached;
	at = now;
	cached.reset();
	JsonObject *o = waybox_request("{\"request\":\"get_windows\"}");
	if (o == nullptr)
		return cached;
	if (json_object_has_member(o, "windows")) {
		std::vector<OpenWindow> list;
		JsonArray *a = json_object_get_array_member(o, "windows");
		for (guint i = 0; a != nullptr && i < json_array_get_length(a); ++i) {
			JsonObject *w = json_array_get_object_element(a, i);
			list.push_back({json_object_get_string_member_with_default(w, "app_id", ""),
					json_object_get_string_member_with_default(w, "title", ""),
					json_object_get_string_member_with_default(w, "tag", "")});
		}
		cached = std::move(list);
	}
	json_object_unref(o);
	return cached;
}

/* "Matches now: foot (~/src), ...": the open windows this rule is the one
 * for; those an earlier rule takes first said so. */
void refresh_matches(RuleEdit *e) {
	if (e->matches == nullptr || !e->now)
		return;
	const auto &wins = open_windows();
	if (!wins) {
		gtk_widget_set_visible(gtk_widget_get_parent(e->matches), FALSE);
		return;
	}
	gtk_widget_set_visible(gtk_widget_get_parent(e->matches), TRUE);
	/* The rules as they'll be, in order, this one as it is now. */
	std::vector<wb::AppRule> rules;
	std::vector<RuleEdit *> owner;
	for (auto &p : e->ui->rule_edits)
		if (p->now && !(p.get() != e && !p->orig && *p->now == wb::AppRule{})) {
			rules.push_back(*p->now);
			owner.push_back(p.get());
		}
	std::vector<std::string> mine, taken;
	for (const OpenWindow &w : *wins) {
		std::vector<wb::AppRule> just = {*e->now};
		if (wb::match_app_rule(just, w.app_id, w.title, w.tag) == nullptr)
			continue;
		const wb::AppRule *first = wb::match_app_rule(rules, w.app_id, w.title, w.tag);
		size_t i = first != nullptr ? static_cast<size_t>(first - rules.data()) : rules.size();
		std::string name = w.app_id.empty() ? std::string(_("(no app id)")) : w.app_id;
		if (!w.title.empty())
			name += " (" + w.title + ")";
		if (i < owner.size() && owner[i] == e) {
			mine.push_back(name);
		} else {
			int n = 0;
			for (auto &p : e->ui->rule_edits) {
				if (p->now)
					++n;
				if (i < owner.size() && p.get() == owner[i])
					break;
			}
			char *t = g_strdup_printf(_("%s, but rule %d is used first"), name.c_str(), n);
			taken.push_back(t);
			g_free(t);
		}
	}
	std::vector<std::string> all = mine;
	all.insert(all.end(), taken.begin(), taken.end());
	std::string text = all.empty() ? std::string(_("No open window.")) : join(all, "; ");
	gtk_label_set_text(GTK_LABEL(e->matches), text.c_str());
}

/* The row's line: what it matches and does, as now (or as it was, struck
 * out). */
void refresh_head(RuleEdit *e) {
	const wb::AppRule &r = shown(e);
	clear(e->match_cell);
	gtk_box_append(GTK_BOX(e->match_cell), match_widget(r));
	std::vector<std::string> effects = wb::app_rule_effects(r);
	std::string does = effects.empty() ? std::string(_("Nothing set yet")) : join(effects, " · ");
	gtk_label_set_text(GTK_LABEL(e->does), does.c_str());
	if (effects.empty())
		gtk_widget_add_css_class(e->does, "dim-label");
	else
		gtk_widget_remove_css_class(e->does, "dim-label");
	std::string err = wb::app_rule_pattern_error(r);
	gtk_label_set_text(GTK_LABEL(e->error_label), (std::string(_("Never matches: a pattern "
			"isn't a regular expression (")) + err + ")").c_str());
	gtk_widget_set_visible(e->error_label, !err.empty());
	if (!e->now)
		gtk_widget_add_css_class(e->head, "wb-removed");
	else
		gtk_widget_remove_css_class(e->head, "wb-removed");
	if (dirty(e) && !incomplete(e))
		gtk_widget_add_css_class(e->outer, "wb-shortcut-unsaved");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-unsaved");
	gtk_widget_set_visible(e->undo, dirty(e) && e->was.has_value());
	gtk_widget_set_tooltip_text(e->undo, !e->now ? _("Keep this rule")
			: _("Put back the rule as saved"));
}

/* ---- The editor's fields ---- */

std::string text_of(GtkWidget *entry) {
	std::string s = gtk_editable_get_text(GTK_EDITABLE(entry));
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::optional<int> int_of(GtkWidget *entry, bool may_be_negative) {
	std::string s = text_of(entry);
	if (s.empty())
		return std::nullopt;
	size_t i = may_be_negative && s[0] == '-' ? 1 : 0;
	if (i >= s.size() || s.find_first_not_of("0123456789", i) != std::string::npos)
		return std::nullopt;
	return atoi(s.c_str());
}

std::optional<bool> tri_of(GtkWidget *dd) {
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	if (i == 1)
		return true;
	if (i == 2)
		return false;
	return std::nullopt;
}

void refresh_hint(RuleEdit *e) {
	if (e->hint == nullptr || !e->now)
		return;
	gtk_label_set_text(GTK_LABEL(e->hint), e->now->patterns == wb::AppRule::Patterns::Glob
			? _("* matches anything, ? one character; each pattern the whole text. A blank "
					"field matches anything.")
			: _("Regular expressions, as egrep reads them: anywhere in the text unless "
					"anchored with ^ and $. A blank field matches anything."));
	std::string err = wb::app_rule_pattern_error(*e->now);
	gtk_label_set_text(GTK_LABEL(e->editor_error), (std::string(_("Not a regular expression: ")) +
			err).c_str());
	/* Its whole line, so no gap is left when there's nothing to say. */
	gtk_widget_set_visible(gtk_widget_get_parent(e->editor_error), !err.empty());
}

/* Every field into the rule, as it is now. */
void read_fields(RuleEdit *e) {
	if (e->updating || !e->now)
		return;
	wb::AppRule &r = *e->now;
	(e->program_is_name ? r.name_pattern : r.class_pattern) = text_of(e->program);
	r.title_pattern = text_of(e->title);
	r.tag_pattern = text_of(e->tag);
	guint k = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->kind));
	r.patterns = k == 1 ? wb::AppRule::Patterns::Regex : k == 2 ? wb::AppRule::Patterns::IRegex
			: wb::AppRule::Patterns::Glob;
	guint d = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->desktop));
	std::string dv = d < e->desktop_values.size() ? e->desktop_values[d] : "";
	if (dv.empty())
		r.desktop.reset();
	else if (dv == "all")
		r.desktop = wb::AppRule::kAllDesktops;
	else
		r.desktop = atoi(dv.c_str()) - 1;
	r.width = int_of(e->width, false);
	r.height = int_of(e->height, false);
	if (r.width && *r.width < 1)
		r.width.reset();
	if (r.height && *r.height < 1)
		r.height.reset();
	if (!e->position_kept) {
		/* A plain number, or Openbox's "center", "50%", "-10". */
		auto coord = [](GtkWidget *w, std::optional<int> &n, std::string &spec) {
			std::string t = text_of(w);
			n.reset();
			spec.clear();
			if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos)
				n = atoi(t.c_str());
			else if (wb::position_spec_ok(t))
				spec = t;
		};
		coord(e->x, r.x, r.x_spec);
		coord(e->y, r.y, r.y_spec);
	}
	guint mi = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->monitor));
	r.monitor = mi < e->monitor_values.size() ? e->monitor_values[mi] : "";
	r.maximized = tri_of(e->tri["maximized"]);
	r.fullscreen = tri_of(e->tri["fullscreen"]);
	r.iconic = tri_of(e->tri["iconic"]);
	r.decor = tri_of(e->tri["decor"]);
	r.focus = tri_of(e->tri["focus"]);
	guint l = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->layer));
	if (l == 1)
		r.layer = 1;
	else if (l == 2)
		r.layer = 0;
	else if (l == 3)
		r.layer = -1;
	else
		r.layer.reset();
	refresh_hint(e);
	refresh_matches(e);
	refresh_head(e);
	refresh_save_state(e->ui);
}

void on_field(GtkWidget *, gpointer data) {
	read_fields(static_cast<RuleEdit *>(data));
}

void on_dd(GObject *, GParamSpec *, gpointer data) {
	read_fields(static_cast<RuleEdit *>(data));
}

/* The fields as the rule is now. */
void show_fields(RuleEdit *e) {
	if (!e->now || e->program == nullptr)
		return;
	const wb::AppRule &r = *e->now;
	e->updating = true;
	gtk_editable_set_text(GTK_EDITABLE(e->program),
			(e->program_is_name ? r.name_pattern : r.class_pattern).c_str());
	gtk_editable_set_text(GTK_EDITABLE(e->title), r.title_pattern.c_str());
	gtk_editable_set_text(GTK_EDITABLE(e->tag), r.tag_pattern.c_str());
	gtk_drop_down_set_selected(GTK_DROP_DOWN(e->kind),
			r.patterns == wb::AppRule::Patterns::Regex ? 1
			: r.patterns == wb::AppRule::Patterns::IRegex ? 2 : 0);
	std::string dv = !r.desktop ? "" : *r.desktop == wb::AppRule::kAllDesktops ? "all"
			: std::to_string(*r.desktop + 1);
	for (size_t i = 0; i < e->desktop_values.size(); ++i)
		if (e->desktop_values[i] == dv)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(e->desktop), static_cast<guint>(i));
	auto num = [](const std::optional<int> &n) { return n ? std::to_string(*n) : std::string(); };
	gtk_editable_set_text(GTK_EDITABLE(e->width), num(r.width).c_str());
	gtk_editable_set_text(GTK_EDITABLE(e->height), num(r.height).c_str());
	gtk_editable_set_text(GTK_EDITABLE(e->x), (r.x ? num(r.x) : r.x_spec).c_str());
	gtk_editable_set_text(GTK_EDITABLE(e->y), (r.y ? num(r.y) : r.y_spec).c_str());
	for (size_t i = 0; i < e->monitor_values.size(); ++i)
		if (e->monitor_values[i] == r.monitor)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(e->monitor), static_cast<guint>(i));
	auto tri = [](GtkWidget *dd, const std::optional<bool> &b) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), !b ? 0 : *b ? 1 : 2);
	};
	tri(e->tri["maximized"], r.maximized);
	tri(e->tri["fullscreen"], r.fullscreen);
	tri(e->tri["iconic"], r.iconic);
	tri(e->tri["decor"], r.decor);
	tri(e->tri["focus"], r.focus);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(e->layer), !r.layer ? 0 : *r.layer > 0 ? 1
			: *r.layer == 0 ? 2 : 3);
	e->updating = false;
	refresh_hint(e);
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

GtkWidget *entry(RuleEdit *e, int chars, const char *placeholder, bool mono) {
	GtkWidget *w = gtk_entry_new();
	gtk_editable_set_width_chars(GTK_EDITABLE(w), chars);
	gtk_editable_set_max_width_chars(GTK_EDITABLE(w), chars);
	if (placeholder != nullptr)
		gtk_entry_set_placeholder_text(GTK_ENTRY(w), placeholder);
	if (mono)
		gtk_widget_add_css_class(w, "monospace");
	g_signal_connect(w, "changed", G_CALLBACK(on_field), e);
	return w;
}

GtkWidget *choices(RuleEdit *e, std::initializer_list<const char *> labels) {
	std::vector<const char *> v(labels);
	v.push_back(nullptr);
	GtkWidget *dd = gtk_drop_down_new_from_strings(v.data());
	g_signal_connect(dd, "notify::selected", G_CALLBACK(on_dd), e);
	return dd;
}

void set_open(RuleEdit *e, bool open) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(e->revealer), open);
	if (open)
		refresh_matches(e);
	if (open)
		gtk_widget_add_css_class(e->outer, "wb-shortcut-open");
	else
		gtk_widget_remove_css_class(e->outer, "wb-shortcut-open");
}

void refresh_empty(Ui *ui) {
	if (ui->rules_empty != nullptr)
		gtk_widget_set_visible(ui->rules_empty, ui->rule_edits.empty());
}

void forget(RuleEdit *e) {
	Ui *ui = e->ui;
	gtk_box_remove(GTK_BOX(ui->rules_box), e->outer);
	auto it = std::find_if(ui->rule_edits.begin(), ui->rule_edits.end(),
			[e](const auto &p) { return p.get() == e; });
	if (it != ui->rule_edits.end())
		ui->rule_edits.erase(it);  /* e is gone from here on */
	refresh_numbers(ui);
	refresh_empty(ui);
	refresh_save_state(ui);
}

void on_remove(GtkButton *, gpointer data) {
	auto *e = static_cast<RuleEdit *>(data);
	set_open(e, false);
	if (!e->was) {
		forget(e);
		return;
	}
	e->now.reset();
	refresh_head(e);
	refresh_numbers(e->ui);
	refresh_save_state(e->ui);
}

void on_undo(GtkButton *, gpointer data) {
	auto *e = static_cast<RuleEdit *>(data);
	set_open(e, false);
	e->now = e->was;
	show_fields(e);
	refresh_head(e);
	refresh_numbers(e->ui);
	refresh_save_state(e->ui);
}

/* Earlier (-1) or later (+1) in the list: the order they're tried in. */
void move(RuleEdit *e, int by) {
	Ui *ui = e->ui;
	auto &v = ui->rule_edits;
	auto it = std::find_if(v.begin(), v.end(), [e](const auto &p) { return p.get() == e; });
	if (it == v.end())
		return;
	long i = it - v.begin(), j = i + by;
	if (j < 0 || j >= static_cast<long>(v.size()))
		return;
	std::swap(v[static_cast<size_t>(i)], v[static_cast<size_t>(j)]);
	/* The rows in the box, in the list's order (after the empty note). */
	GtkWidget *prev = ui->rules_empty;
	for (auto &p : v) {
		gtk_box_reorder_child_after(GTK_BOX(ui->rules_box), p->outer, prev);
		prev = p->outer;
	}
	refresh_numbers(ui);
	refresh_save_state(ui);
}

/* A title as a pattern that matches it exactly: regex metacharacters
 * escaped; a glob can't escape * or ?, so they stay (and match more). */
std::string literal(const std::string &text, wb::AppRule::Patterns kind) {
	if (kind == wb::AppRule::Patterns::Glob)
		return text;
	std::string out = "^";
	for (char c : text) {
		if (std::string_view(".[]{}()\\*+?^$|").find(c) != std::string_view::npos)
			out += '\\';
		out += c;
	}
	return out + "$";
}

void refresh_pick(RuleEdit *e) {
	gtk_button_set_label(GTK_BUTTON(e->pick), e->picking ? _("Click a window\u2026")
			: _("Pick a Window"));
	gtk_widget_set_sensitive(e->pick, !e->picking);
}

/* Pick a Window: Waybox's next click names a window; its app id (and tag)
 * fill the rule in, and its title is offered. */
void on_pick(GtkButton *, gpointer data) {
	auto *e = static_cast<RuleEdit *>(data);
	if (e->picking)
		return;
	e->picking = true;
	refresh_pick(e);
	GTask *task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *res, gpointer d) {
		auto *weak = static_cast<std::weak_ptr<RuleEdit> *>(d);
		auto *o = static_cast<JsonObject *>(g_task_propagate_pointer(G_TASK(res), nullptr));
		if (std::shared_ptr<RuleEdit> e = weak->lock(); e && e->now) {
			e->picking = false;
			refresh_pick(e.get());
			JsonObject *w = o != nullptr && json_object_has_member(o, "window")
					? json_object_get_object_member(o, "window") : nullptr;
			if (w != nullptr) {
				std::string app = json_object_get_string_member_with_default(w, "app_id", "");
				std::string tag = json_object_get_string_member_with_default(w, "tag", "");
				e->picked_title = json_object_get_string_member_with_default(w, "title", "");
				gtk_editable_set_text(GTK_EDITABLE(e->program),
						literal(app, e->now->patterns).c_str());
				if (!tag.empty())
					gtk_editable_set_text(GTK_EDITABLE(e->tag), literal(tag, e->now->patterns).c_str());
				gtk_widget_set_visible(e->use_title, !e->picked_title.empty());
				gtk_widget_set_tooltip_text(e->use_title, e->picked_title.c_str());
			}
		}
		delete weak;
		if (o != nullptr)
			json_object_unref(o);
	}, new std::weak_ptr<RuleEdit>(e->self));
	g_task_run_in_thread(task, [](GTask *task, gpointer, gpointer, GCancellable *) {
		/* Waybox gives up after 30 s: wait a little longer. */
		g_task_return_pointer(task, waybox_request("{\"request\":\"pick_window\"}", 35),
				[](gpointer o) { json_object_unref(static_cast<JsonObject *>(o)); });
	});
	g_object_unref(task);
}

GtkWidget *build_editor(RuleEdit *e) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "wb-shortcut-editor");
	GtkSizeGroup *labels = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	g_object_set_data_full(G_OBJECT(box), "wb-labels", labels, g_object_unref);

	/* What it matches. */
	e->program = entry(e, 24, _("Any program"), true);
	gtk_widget_set_tooltip_text(e->program, _("The program's app id (foot, firefox, "
			"org.gnome.Nautilus)"));
	GtkWidget *prog = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(prog), e->program);
	e->pick = gtk_button_new_with_label(_("Pick a Window"));
	gtk_widget_set_tooltip_text(e->pick, _("Click a window to fill in its program (Esc or a "
			"right click cancels)"));
	g_signal_connect(e->pick, "clicked", G_CALLBACK(on_pick), e);
	gtk_box_append(GTK_BOX(prog), e->pick);
	gtk_box_append(GTK_BOX(box), field(labels, _("Program"), prog));
	e->title = entry(e, 24, _("Any title"), true);
	GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(titles), e->title);
	e->use_title = gtk_button_new_with_label(_("Use Its Title"));
	gtk_widget_add_css_class(e->use_title, "flat");
	gtk_widget_set_visible(e->use_title, FALSE);
	g_signal_connect(e->use_title, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		auto *x = static_cast<RuleEdit *>(d);
		if (x->now)
			gtk_editable_set_text(GTK_EDITABLE(x->title),
					literal(x->picked_title, x->now->patterns).c_str());
	}), e);
	gtk_box_append(GTK_BOX(titles), e->use_title);
	gtk_box_append(GTK_BOX(box), field(labels, _("Title"), titles));
	e->tag = entry(e, 24, _("Any tag"), true);
	gtk_widget_set_tooltip_text(e->tag, _("A tag some programs give their windows "
			"(xdg-toplevel-tag)"));
	gtk_box_append(GTK_BOX(box), field(labels, _("Tag"), e->tag));
	e->kind = choices(e, {_("Simple (* and ?)"), _("Regular expression"),
			_("Regular expression, any case")});
	gtk_box_append(GTK_BOX(box), field(labels, _("Patterns"), e->kind));
	e->hint = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->hint), 0);
	gtk_label_set_wrap(GTK_LABEL(e->hint), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(e->hint), 70);
	gtk_label_set_width_chars(GTK_LABEL(e->hint), 50);
	gtk_widget_add_css_class(e->hint, "dim-label");
	gtk_box_append(GTK_BOX(box), field(labels, "", e->hint));
	e->editor_error = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->editor_error), 0);
	gtk_widget_add_css_class(e->editor_error, "wb-rule-error");
	gtk_box_append(GTK_BOX(box), field(labels, "", e->editor_error));
	e->matches = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->matches), 0);
	gtk_label_set_wrap(GTK_LABEL(e->matches), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(e->matches), 70);
	gtk_label_set_width_chars(GTK_LABEL(e->matches), 50);
	gtk_widget_add_css_class(e->matches, "wb-rule-matches");
	gtk_box_append(GTK_BOX(box), field(labels, _("Matches now"), e->matches));

	GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top(sep, 4);
	gtk_widget_set_margin_bottom(sep, 4);
	gtk_box_append(GTK_BOX(box), sep);

	/* What it does: "As usual" leaves each to Waybox. */
	GtkStringList *desks = gtk_string_list_new(nullptr);
	gtk_string_list_append(desks, _("As usual"));
	e->desktop_values.push_back("");
	gtk_string_list_append(desks, _("Every desktop"));
	e->desktop_values.push_back("all");
	int n = desktop_count(e->ui);
	if (e->now && e->now->desktop && *e->now->desktop + 1 > n)
		n = *e->now->desktop + 1;
	for (int i = 1; i <= n; ++i) {
		gtk_string_list_append(desks, (std::string(_("Desktop ")) + std::to_string(i)).c_str());
		e->desktop_values.push_back(std::to_string(i));
	}
	e->desktop = gtk_drop_down_new(G_LIST_MODEL(desks), nullptr);
	g_signal_connect(e->desktop, "notify::selected", G_CALLBACK(on_dd), e);
	gtk_box_append(GTK_BOX(box), field(labels, _("Desktop"), e->desktop));

	GtkWidget *size = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	e->width = entry(e, 6, _("auto"), false);
	e->height = entry(e, 6, _("auto"), false);
	gtk_box_append(GTK_BOX(size), e->width);
	gtk_box_append(GTK_BOX(size), dim("×"));
	gtk_box_append(GTK_BOX(size), e->height);
	gtk_box_append(GTK_BOX(box), field(labels, _("Size"), size));

	GtkWidget *pos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	e->x = entry(e, 6, _("auto"), false);
	e->y = entry(e, 6, _("auto"), false);
	gtk_box_append(GTK_BOX(pos), e->x);
	gtk_box_append(GTK_BOX(pos), dim(","));
	gtk_box_append(GTK_BOX(pos), e->y);
	if (e->position_kept) {
		gtk_widget_set_sensitive(e->x, FALSE);
		gtk_widget_set_sensitive(e->y, FALSE);
		gtk_box_append(GTK_BOX(pos), dim(_("as written in rc.xml")));
	} else {
		gtk_widget_set_tooltip_text(e->x, _("Pixels from the left; or center, 50%, or -10 "
				"(10 from the right edge)"));
		gtk_widget_set_tooltip_text(e->y, _("Pixels from the top; or center, 50%, or -10 "
				"(10 from the bottom edge)"));
	}
	gtk_box_append(GTK_BOX(box), field(labels, _("Position"), pos));

	/* Which monitor: as usual (the pointer's), or a given one. */
	GtkStringList *mons = gtk_string_list_new(nullptr);
	auto add_mon = [&](const char *label, const std::string &value) {
		gtk_string_list_append(mons, label);
		e->monitor_values.push_back(value);
	};
	add_mon(_("As usual"), "");
	add_mon(_("The one with the pointer"), "mouse");
	add_mon(_("The focused window's"), "active");
	/* The monitors connected, left to right, by name (a name stays put
	 * when they're plugged in again; a number counts by place). */
	std::vector<std::pair<std::string, std::string>> named;  /* name, description */
	if (JsonObject *o = waybox_request("{\"request\":\"get_outputs\"}")) {
		std::vector<wb::MonitorPlace> places;
		std::vector<std::string> descs;
		JsonArray *a = json_object_has_member(o, "outputs")
				? json_object_get_array_member(o, "outputs") : nullptr;
		for (guint i = 0; a != nullptr && i < json_array_get_length(a); ++i) {
			JsonObject *m = json_array_get_object_element(a, i);
			places.push_back({json_object_get_string_member_with_default(m, "name", ""),
					static_cast<int>(json_object_get_int_member_with_default(m, "x", 0)),
					static_cast<int>(json_object_get_int_member_with_default(m, "y", 0))});
			descs.push_back(json_object_get_string_member_with_default(m, "description", ""));
		}
		for (size_t n = 1; n <= places.size(); ++n)
			if (auto i = wb::rule_monitor(places, std::to_string(n)))
				named.emplace_back(places[*i].name, descs[*i]);
		json_object_unref(o);
	}
	for (const auto &[name, desc] : named)
		add_mon((name + (desc.empty() ? "" : " \u2014 " + desc)).c_str(), name);
	if (named.empty())
		for (int i = 1; i <= 4; ++i)
			add_mon((std::string(_("Monitor ")) + std::to_string(i)).c_str(), std::to_string(i));
	/* What the rule has, if not among those (a number; one not connected). */
	if (e->now && !e->now->monitor.empty() && std::find(e->monitor_values.begin(),
			e->monitor_values.end(), e->now->monitor) == e->monitor_values.end()) {
		const std::string &m = e->now->monitor;
		std::string label = m == "primary" ? std::string(_("The first (primary)"))
				: m.find_first_not_of("0123456789") == std::string::npos
				? std::string(_("Monitor ")) + m + " " + _("(counted left to right)")
				: m + " " + _("(not connected now)");
		add_mon(label.c_str(), m);
	}
	e->monitor = gtk_drop_down_new(G_LIST_MODEL(mons), nullptr);
	g_signal_connect(e->monitor, "notify::selected", G_CALLBACK(on_dd), e);
	gtk_box_append(GTK_BOX(box), field(labels, _("Monitor"), e->monitor));

	struct Tri { const char *key, *label, *yes, *no; };
	for (const Tri &t : {Tri{"maximized", _("Maximized"), _("Yes"), _("No")},
			Tri{"fullscreen", _("Full screen"), _("Yes"), _("No")},
			Tri{"iconic", _("Starts minimized"), _("Yes"), _("No")},
			Tri{"decor", _("Title bar"), _("Shown"), _("Hidden")},
			Tri{"focus", _("Focus when it opens"), _("Yes"), _("No")}}) {
		e->tri[t.key] = choices(e, {_("As usual"), t.yes, t.no});
		gtk_box_append(GTK_BOX(box), field(labels, t.label, e->tri[t.key]));
	}
	e->layer = choices(e, {_("As usual"), _("Always on top"), _("Normal"), _("Always below")});
	gtk_box_append(GTK_BOX(box), field(labels, _("Layer"), e->layer));

	if (!e->kept.empty()) {
		GtkWidget *k = dim(std::string(_("Also kept, as written in rc.xml: ")) +
				join(e->kept, ", "));
		gtk_label_set_xalign(GTK_LABEL(k), 0);
		gtk_label_set_wrap(GTK_LABEL(k), TRUE);
		gtk_box_append(GTK_BOX(box), k);
	}

	/* Remove on the left; order and Done on the right. */
	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(foot, 4);
	GtkWidget *remove = gtk_button_new_with_label(_("Remove Rule"));
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_add_css_class(remove, "wb-remove");
	g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), e);
	gtk_box_append(GTK_BOX(foot), remove);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	e->earlier = gtk_button_new_from_icon_name("go-up-symbolic");
	gtk_widget_add_css_class(e->earlier, "flat");
	gtk_widget_set_tooltip_text(e->earlier, _("Move Earlier: tried before the rule above"));
	g_signal_connect(e->earlier, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		move(static_cast<RuleEdit *>(d), -1);
	}), e);
	gtk_box_append(GTK_BOX(foot), e->earlier);
	e->later = gtk_button_new_from_icon_name("go-down-symbolic");
	gtk_widget_add_css_class(e->later, "flat");
	gtk_widget_set_tooltip_text(e->later, _("Move Later: tried after the rule below"));
	g_signal_connect(e->later, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		move(static_cast<RuleEdit *>(d), 1);
	}), e);
	gtk_box_append(GTK_BOX(foot), e->later);
	GtkWidget *done = gtk_button_new_with_label(_("Done"));
	g_signal_connect(done, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		set_open(static_cast<RuleEdit *>(d), false);
	}), e);
	gtk_box_append(GTK_BOX(foot), done);
	gtk_box_append(GTK_BOX(box), foot);
	show_fields(e);
	return box;
}

RuleEdit *add_row(Ui *ui, std::shared_ptr<RuleEdit> e) {
	e->ui = ui;
	e->self = e;
	if (e->now)
		e->program_is_name = e->now->class_pattern.empty() && !e->now->name_pattern.empty();
	e->outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	e->number = gtk_label_new("");
	gtk_widget_add_css_class(e->number, "wb-rule-order");
	gtk_widget_set_valign(e->number, GTK_ALIGN_START);
	gtk_widget_set_tooltip_text(e->number, _("Rules are tried in this order; the first that "
			"matches a window is the one used"));
	gtk_box_append(GTK_BOX(line), e->number);
	e->match_cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_valign(e->match_cell, GTK_ALIGN_START);
	gtk_size_group_add_widget(ui->rules_column, e->match_cell);
	gtk_box_append(GTK_BOX(line), e->match_cell);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(right, TRUE);
	e->does = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->does), 0);
	gtk_label_set_wrap(GTK_LABEL(e->does), TRUE);
	gtk_box_append(GTK_BOX(right), e->does);
	e->error_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(e->error_label), 0);
	gtk_label_set_wrap(GTK_LABEL(e->error_label), TRUE);
	gtk_widget_add_css_class(e->error_label, "wb-rule-error");
	gtk_box_append(GTK_BOX(right), e->error_label);
	if (!e->kept.empty() || e->position_kept) {
		std::vector<std::string> kept = e->kept;
		if (e->position_kept)
			kept.insert(kept.begin(), "position");
		e->kept_label = dim(std::string(_("Also, as written in rc.xml: ")) + join(kept, ", "));
		gtk_label_set_xalign(GTK_LABEL(e->kept_label), 0);
		gtk_label_set_wrap(GTK_LABEL(e->kept_label), TRUE);
		gtk_widget_add_css_class(e->kept_label, "wb-rule-kept");
		gtk_widget_set_tooltip_text(e->kept_label, _("Openbox settings Waybox doesn't use: kept "
				"in rc.xml as they are"));
		gtk_box_append(GTK_BOX(right), e->kept_label);
	}
	gtk_box_append(GTK_BOX(line), right);

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	e->head = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(e->head), line);
	gtk_widget_add_css_class(e->head, "flat");
	gtk_widget_add_css_class(e->head, "wb-shortcut-row");
	gtk_widget_add_css_class(e->head, "wb-rule-row");
	gtk_widget_set_hexpand(e->head, TRUE);
	g_signal_connect(e->head, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		auto *x = static_cast<RuleEdit *>(d);
		if (x->now)
			set_open(x, !gtk_revealer_get_reveal_child(GTK_REVEALER(x->revealer)));
	}), e.get());
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
	if (e->now)
		gtk_revealer_set_child(GTK_REVEALER(e->revealer), build_editor(e.get()));
	gtk_box_append(GTK_BOX(e->outer), e->revealer);

	/* At the end of the list, before its foot. */
	gtk_box_insert_child_after(GTK_BOX(ui->rules_box), e->outer,
			gtk_widget_get_prev_sibling(ui->rules_foot));
	ui->rule_edits.push_back(e);
	refresh_head(e.get());
	return e.get();
}

void refresh_numbers(Ui *ui) {
	int n = 0;
	for (auto &e : ui->rule_edits) {
		/* Removed rules keep a place in the list, not a number. */
		gtk_label_set_text(GTK_LABEL(e->number), e->now ? std::to_string(++n).c_str() : "");
		gtk_widget_set_opacity(e->number, e->now ? 1.0 : 0.0);
	}
	for (size_t i = 0; i < ui->rule_edits.size(); ++i) {
		RuleEdit *e = ui->rule_edits[i].get();
		if (e->earlier != nullptr) {
			gtk_widget_set_sensitive(e->earlier, i > 0);
			gtk_widget_set_sensitive(e->later, i + 1 < ui->rule_edits.size());
		}
	}
}

RuleEdit *add_new(Ui *ui) {
	auto e = std::make_shared<RuleEdit>();
	e->now = wb::AppRule{};
	RuleEdit *r = add_row(ui, e);
	refresh_numbers(ui);
	refresh_empty(ui);
	set_open(r, true);
	gtk_widget_grab_focus(r->program);
	refresh_save_state(ui);
	return r;
}

/* The rules kept, in an order other than as saved. */
bool order_changed(Ui *ui) {
	std::vector<size_t> origs;
	bool seen_new = false;
	for (auto &e : ui->rule_edits) {
		if (!e->now || incomplete(e.get()))
			continue;
		if (!e->orig) {
			seen_new = true;
			continue;
		}
		/* A saved rule after a new one: the new one moves it on. */
		if (seen_new)
			return true;
		origs.push_back(*e->orig);
	}
	return !std::is_sorted(origs.begin(), origs.end());
}

RuleEdit *nth(Ui *ui, const std::string &n) {
	size_t i = static_cast<size_t>(atoi(n.c_str()));
	return i >= 1 && i <= ui->rule_edits.size() ? ui->rule_edits[i - 1].get() : nullptr;
}

}  // namespace

GtkWidget *build_rules_block(Ui *ui) {
	ui->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	return ui->rules_box;
}

void populate_rules(Ui *ui, const wb::RcDocument *doc) {
	if (doc == nullptr || ui->rules_box == nullptr)
		return;
	clear(ui->rules_box);
	ui->rule_edits.clear();
	ui->rules_script_note.clear();
	if (ui->rules_column != nullptr)
		g_object_unref(ui->rules_column);
	ui->rules_column = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	ui->rules_empty = dim(_("No rules yet. A rule sets how a program's windows open: on which "
			"desktop, where and how big, maximized or full screen, with or without a "
			"title bar."));
	gtk_label_set_wrap(GTK_LABEL(ui->rules_empty), TRUE);
	gtk_label_set_xalign(GTK_LABEL(ui->rules_empty), 0);
	gtk_widget_add_css_class(ui->rules_empty, "wb-rules-empty");
	gtk_box_append(GTK_BOX(ui->rules_box), ui->rules_empty);

	/* A script's rules policy (wb.policy "rules") goes over these, setting
	 * by setting: said first. */
	for (const auto &[name, text] : script_sources(doc))
		for (const wb::ScriptKey &c : wb::script_calls(text, name, "wb.policy"))
			if (c.key == "rules") {
				char *t = g_strdup_printf(_("%s (line %d) sets a rules policy: what it sets for "
						"a window wins over these rules, setting by setting."), c.file.c_str(),
						c.line);
				GtkWidget *note = gtk_label_new(t);
				g_free(t);
				gtk_label_set_xalign(GTK_LABEL(note), 0);
				gtk_label_set_wrap(GTK_LABEL(note), TRUE);
				gtk_widget_add_css_class(note, "wb-rules-script");
				gtk_box_prepend(GTK_BOX(ui->rules_box), note);
				ui->rules_script_note = std::string(c.file) + ":" + std::to_string(c.line);
			}

	ui->rules_foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(ui->rules_foot, 8);
	GtkWidget *add = gtk_button_new();
	GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(inside), gtk_image_new_from_icon_name("list-add-symbolic"));
	gtk_box_append(GTK_BOX(inside), gtk_label_new(_("Add a Rule")));
	gtk_button_set_child(GTK_BUTTON(add), inside);
	gtk_widget_add_css_class(add, "flat");
	g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		add_new(static_cast<Ui *>(d));
	}), ui);
	gtk_box_append(GTK_BOX(ui->rules_foot), add);
	gtk_box_append(GTK_BOX(ui->rules_box), ui->rules_foot);

	std::vector<wb::AppRuleEntry> rules = doc->app_rules();
	for (size_t i = 0; i < rules.size(); ++i) {
		auto e = std::make_shared<RuleEdit>();
		e->was = rules[i].rule;
		e->now = rules[i].rule;
		e->orig = i;
		e->kept = rules[i].kept;
		e->position_kept = rules[i].position_kept;
		add_row(ui, e);
	}
	refresh_numbers(ui);
	refresh_empty(ui);
}

void apply_rules(Ui *ui, wb::RcDocument &doc) {
	auto &v = ui->rule_edits;
	/* 1. Changed in place (indices as loaded still hold). */
	for (auto &e : v)
		if (e->orig && e->now && dirty(e.get()))
			doc.set_app_rule(*e->orig, *e->now);
	/* 2. New ones at the end, in the list's order. */
	std::vector<RuleEdit *> in_doc;
	for (auto &e : v)
		if (e->orig && e->now)
			in_doc.push_back(e.get());
	std::sort(in_doc.begin(), in_doc.end(), [](RuleEdit *a, RuleEdit *b) {
		return *a->orig < *b->orig;
	});
	for (auto &e : v)
		if (!e->orig && e->now && !incomplete(e.get())) {
			doc.set_app_rule(std::nullopt, *e->now);
			in_doc.push_back(e.get());
		}
	/* 3. Removed, from the last up (indices before it hold). */
	std::vector<size_t> gone;
	for (auto &e : v)
		if (e->orig && !e->now)
			gone.push_back(*e->orig);
	std::sort(gone.rbegin(), gone.rend());
	for (size_t i : gone)
		doc.set_app_rule(i, std::nullopt);
	/* 4. In the list's order. */
	std::vector<RuleEdit *> want;
	for (auto &e : v)
		if (e->now && !incomplete(e.get()))
			want.push_back(e.get());
	for (size_t t = 0; t < want.size() && t < in_doc.size(); ++t) {
		size_t at = static_cast<size_t>(std::find(in_doc.begin(), in_doc.end(), want[t]) -
				in_doc.begin());
		if (at == t || at >= in_doc.size())
			continue;
		doc.move_app_rule(at, t);
		RuleEdit *r = in_doc[at];
		in_doc.erase(in_doc.begin() + static_cast<long>(at));
		in_doc.insert(in_doc.begin() + static_cast<long>(t), r);
	}
}

void rule_changes(Ui *ui, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out) {
	for (auto &e : ui->rule_edits) {
		if (!dirty(e.get()) || incomplete(e.get()))
			continue;
		out.push_back({{where, rule_text(e->was), rule_text(e->now)}, target});
	}
	if (order_changed(ui)) {
		std::vector<std::string> before, after;
		std::vector<RuleEdit *> saved;
		for (auto &e : ui->rule_edits)
			if (e->orig)
				saved.push_back(e.get());
		std::sort(saved.begin(), saved.end(), [](RuleEdit *a, RuleEdit *b) {
			return *a->orig < *b->orig;
		});
		for (RuleEdit *e : saved)
			before.push_back(match_text(*e->was));
		for (auto &e : ui->rule_edits)
			if (e->now && !incomplete(e.get()))
				after.push_back(match_text(*e->now));
		out.push_back({{where + " \xE2\x80\xBA " + _("Order"), join(before, ", "),
				join(after, ", ")}, target});
	}
}

void rule_dump(Ui *ui) {
	fprintf(stderr, "rule-dump:%s\n", ui->rules_script_note.c_str());
	for (auto &e : ui->rule_edits) {
		const wb::AppRule &r = shown(e.get());
		std::string kept = join(e->kept, ",") + (e->position_kept ? "|position" : "") +
				(r.patterns == wb::AppRule::Patterns::Regex ? "|regex"
				 : r.patterns == wb::AppRule::Patterns::IRegex ? "|iregex" : "") +
				(wb::app_rule_pattern_error(r).empty() ? "" : "|never");
		std::string state = !e->now ? "|removed" : dirty(e.get()) && !incomplete(e.get())
				? "|changed" : "";
		fprintf(stderr, "rule|%s|%s|%s|%s%s\n", r.class_pattern.c_str(), r.title_pattern.c_str(),
				join(wb::app_rule_effects(r), ";").c_str(), kept.c_str(), state.c_str());
	}
}

/* Test steps, through the same widgets a person uses (N: the rule's place
 * in the list, from 1):
 *   rule-open:N   rule-remove:N   rule-undo:N   rule-earlier:N   rule-later:N
 *   rule-set:N=field=value  (program, title, tag; patterns: glob, regex,
 *       iregex; desktop: "", all, 2...; width, height, x, y; maximized,
 *       fullscreen, iconic, decor, focus: "", yes, no; layer: "", above,
 *       normal, below)
 *   rule-add   (a new rule, last)
 */
void rule_test_step(Ui *ui, const std::string &step) {
	size_t colon = step.find(':');
	std::string verb = step.substr(0, colon);
	std::string arg = colon == std::string::npos ? "" : step.substr(colon + 1);
	if (verb == "rule-add") {
		add_new(ui);
		return;
	}
	size_t eq = arg.find('=');
	RuleEdit *e = nth(ui, arg.substr(0, eq));
	if (e == nullptr) {
		fprintf(stderr, "rule-step: no rule %s\n", arg.c_str());
		return;
	}
	if (verb == "rule-open") {
		set_open(e, true);
	} else if (verb == "rule-matches") {
		if (e->matches != nullptr) {
			refresh_matches(e);
			fprintf(stderr, "rule-matches:%s\n", gtk_label_get_text(GTK_LABEL(e->matches)));
		}
	} else if (verb == "rule-pick") {
		on_pick(nullptr, e);
	} else if (verb == "rule-remove") {
		on_remove(nullptr, e);
	} else if (verb == "rule-undo") {
		on_undo(nullptr, e);
	} else if (verb == "rule-earlier") {
		move(e, -1);
	} else if (verb == "rule-later") {
		move(e, 1);
	} else if (verb == "rule-set" && eq != std::string::npos && e->program != nullptr) {
		std::string rest = arg.substr(eq + 1);
		size_t eq2 = rest.find('=');
		std::string f = rest.substr(0, eq2);
		std::string val = eq2 == std::string::npos ? "" : rest.substr(eq2 + 1);
		auto pick = [](GtkWidget *dd, std::initializer_list<const char *> values,
				const std::string &v) {
			guint i = 0;
			for (const char *x : values) {
				if (v == x)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
				++i;
			}
		};
		if (f == "program") {
			gtk_editable_set_text(GTK_EDITABLE(e->program), val.c_str());
		} else if (f == "title") {
			gtk_editable_set_text(GTK_EDITABLE(e->title), val.c_str());
		} else if (f == "tag") {
			gtk_editable_set_text(GTK_EDITABLE(e->tag), val.c_str());
		} else if (f == "patterns") {
			pick(e->kind, {"glob", "regex", "iregex"}, val);
		} else if (f == "desktop") {
			for (size_t i = 0; i < e->desktop_values.size(); ++i)
				if (e->desktop_values[i] == val)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(e->desktop), static_cast<guint>(i));
		} else if (f == "width") {
			gtk_editable_set_text(GTK_EDITABLE(e->width), val.c_str());
		} else if (f == "height") {
			gtk_editable_set_text(GTK_EDITABLE(e->height), val.c_str());
		} else if (f == "x") {
			gtk_editable_set_text(GTK_EDITABLE(e->x), val.c_str());
		} else if (f == "y") {
			gtk_editable_set_text(GTK_EDITABLE(e->y), val.c_str());
		} else if (f == "monitor") {
			for (size_t i = 0; i < e->monitor_values.size(); ++i)
				if (e->monitor_values[i] == val)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(e->monitor), static_cast<guint>(i));
		} else if (f == "layer") {
			pick(e->layer, {"", "above", "normal", "below"}, val);
		} else if (e->tri.count(f)) {
			pick(e->tri[f], {"", "yes", "no"}, val);
		}
	}
}

}  // namespace wbconf
