/*
 * The Menu page's tree (#238): the root menu (menu.xml's "root-menu", the
 * one a right click on the desktop shows), entry by entry, submenus
 * indented under theirs: each with its icon, its label and what it does.
 *
 * An entry is edited where it stands (its row opens underneath): the
 * label and icon, and by kind the action (an item), the title (a
 * separator), the command (a pipe menu) or the menu shown (a reference).
 * Entries are added (a program, separator, submenu, the applications
 * menu, a pipe menu), removed, and moved earlier, later, into the submenu
 * above or out of theirs. Every change is kept as a step on a copy of the
 * file (comments and what the editor doesn't model untouched); Undo takes
 * the last one back, and Save & Apply writes the copy.
 */
#include "wbconf_ui.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "action_editor.hpp"
#include "waybox/menu_doc.hpp"

namespace fs = std::filesystem;

namespace wbconf {

namespace {

using Path = wb::MenuDocument::Path;
using K = wb::MenuEntry::Kind;

/* A change, as made: replayed on the file as loaded to get the copy. */
struct Step {
	enum class Op { Set, Insert, Remove, Move } op;
	Path path;        /* Set, Remove, Move: the entry; Insert: the parent */
	Path to;          /* Move: the new parent */
	size_t index = 0; /* Insert, Move: the place among its entries */
	wb::MenuEntry entry;
	std::string before, after;  /* for the unsaved changes */
};

}  // namespace

struct MenuState {
	std::string path;        /* the file written (the user's menu.xml) */
	std::string loaded;      /* the text it was loaded from */
	std::string problem;     /* why it couldn't be read, if so */
	std::string id = "root-menu";
	std::vector<Step> steps;
	std::optional<wb::MenuDocument> doc;  /* loaded, with the steps done */
	std::optional<Path> open;             /* the entry whose editor is open */
	GtkWidget *tree = nullptr, *undo = nullptr, *editor = nullptr;
};

namespace {

std::string user_menu_path() {
	std::string rc = user_rc_path();
	return rc.empty() ? std::string() : (fs::path(rc).parent_path() / "menu.xml").string();
}

std::string default_menu_path() {
	if (const char *t = getenv("WBCONF_DEFAULT_MENU"); t != nullptr && *t)
		return t;
#ifdef WB_SYSCONFDIR
	return (fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "menu.xml").string();
#else
	return {};
#endif
}

std::string read_text(const std::string &path) {
	std::ifstream in(path);
	if (!in)
		return {};
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

std::string path_text(const Path &p) {
	std::string out;
	for (size_t i : p)
		out += (out.empty() ? "" : ".") + std::to_string(i + 1);
	return out;
}

Path parse_path(const std::string &s) {
	Path out;
	size_t at = 0;
	while (at < s.size()) {
		size_t dot = s.find('.', at);
		out.push_back(static_cast<size_t>(atoi(s.substr(at, dot - at).c_str())) - 1);
		if (dot == std::string::npos)
			break;
		at = dot + 1;
	}
	return out;
}

const wb::MenuEntry *entry_at(const wb::MenuEntry &menu, const Path &p) {
	const wb::MenuEntry *e = &menu;
	for (size_t i : p) {
		if (i >= e->children.size())
			return nullptr;
		e = &e->children[i];
	}
	return e;
}

/* What an entry does, in words. */
std::string does_text(const wb::MenuEntry &e, const wb::MenuDocument &doc) {
	switch (e.kind) {
	case K::Item:
		return e.actions.empty() ? std::string(_("Does nothing")) : wb::describe_actions(e.actions);
	case K::Separator:
		return e.label.empty() ? std::string(_("A divider")) : std::string(_("A title"));
	case K::Submenu: {
		char *t = g_strdup_printf(ngettext("A submenu of %zu entry", "A submenu of %zu entries",
				e.children.size()), e.children.size());
		std::string s = t;
		g_free(t);
		return s;
	}
	case K::Reference: {
		auto target = doc.menu(e.id);
		std::string name = target && !target->label.empty() ? target->label : e.id;
		return std::string(_("Shows the menu ")) + "“" + name + "”";
	}
	case K::Pipe:
		return std::string(_("Entries from ")) + e.execute;
	case K::Applications:
		return _("The installed programs");
	}
	return {};
}

std::string entry_text(const wb::MenuEntry &e, const wb::MenuDocument &doc) {
	std::string label = e.label.empty() && e.kind == K::Separator ? std::string(_("Separator"))
			: e.label;
	return label + ": " + does_text(e, doc);
}

GtkWidget *icon_of(const std::string &icon) {
	GtkWidget *img = icon.empty() ? gtk_image_new() : icon[0] == '/'
			? gtk_image_new_from_file(icon.c_str()) : gtk_image_new_from_icon_name(icon.c_str());
	gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
	gtk_widget_set_size_request(img, 16, 16);
	return img;
}

void render(Ui *ui);
void changed(Ui *ui);

/* The steps replayed on the file as loaded: the copy as it is now. */
void rebuild(MenuState *st) {
	st->doc = wb::MenuDocument::from_string(st->loaded);
	if (!st->doc)
		return;
	for (const Step &s : st->steps) {
		switch (s.op) {
		case Step::Op::Set:
			st->doc->set(st->id, s.path, s.entry);
			break;
		case Step::Op::Insert:
			st->doc->insert(st->id, s.path, s.index, s.entry);
			break;
		case Step::Op::Remove:
			st->doc->remove(st->id, s.path);
			break;
		case Step::Op::Move:
			st->doc->move(st->id, s.path, s.to, s.index);
			break;
		}
	}
}

wb::MenuEntry current(MenuState *st) {
	auto m = st->doc ? st->doc->menu(st->id) : std::nullopt;
	return m ? *m : wb::MenuEntry{};
}

/* A step done: on the copy now, and kept (an edit to the same entry as
 * the last step joins it, so typing a label is one step). */
void step(Ui *ui, Step s) {
	MenuState *st = ui->menu.get();
	if (!st->doc)
		return;
	if (s.op == Step::Op::Set && !st->steps.empty() && st->steps.back().op == Step::Op::Set &&
			st->steps.back().path == s.path) {
		s.before = st->steps.back().before;
		st->steps.back() = s;
	} else {
		st->steps.push_back(s);
	}
	switch (s.op) {
	case Step::Op::Set:
		st->doc->set(st->id, s.path, s.entry);
		break;
	case Step::Op::Insert:
		st->doc->insert(st->id, s.path, s.index, s.entry);
		break;
	case Step::Op::Remove:
		st->doc->remove(st->id, s.path);
		break;
	case Step::Op::Move:
		st->doc->move(st->id, s.path, s.to, s.index);
		break;
	}
	changed(ui);
}

/* A new submenu's id: from its label, not one in use. */
std::string fresh_id(MenuState *st, const std::string &label) {
	std::string base;
	for (char c : label)
		base += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(
				static_cast<unsigned char>(c))) : '-';
	if (base.empty() || base.find_first_not_of('-') == std::string::npos)
		base = "submenu";
	std::string id = base;
	for (int n = 2; st->doc && st->doc->menu(id); ++n)
		id = base + "-" + std::to_string(n);
	return id;
}

/* ---- The editor under an entry ---- */

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

struct EditorCtx {
	Ui *ui;
	Path path;
};

/* The entry at the open editor's path, changed by `edit`, as one step. */
void edit_open(Ui *ui, const std::function<void(wb::MenuEntry &)> &edit) {
	MenuState *st = ui->menu.get();
	if (!st->open || !st->doc)
		return;
	wb::MenuEntry menu = current(st);
	const wb::MenuEntry *e = entry_at(menu, *st->open);
	if (e == nullptr)
		return;
	wb::MenuEntry now = *e;
	edit(now);
	if (now == *e)
		return;
	Step s{Step::Op::Set, *st->open, {}, 0, now, entry_text(*e, *st->doc),
			entry_text(now, *st->doc)};
	step(ui, s);
}

GtkWidget *entry_field(const std::string &text, const char *placeholder, int chars,
		bool mono, std::function<void(const std::string &)> on) {
	GtkWidget *w = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(w), text.c_str());
	gtk_editable_set_width_chars(GTK_EDITABLE(w), chars);
	if (placeholder != nullptr)
		gtk_entry_set_placeholder_text(GTK_ENTRY(w), placeholder);
	if (mono)
		gtk_widget_add_css_class(w, "monospace");
	auto *cb = new std::function<void(const std::string &)>(std::move(on));
	g_object_set_data_full(G_OBJECT(w), "wb-on", cb, [](gpointer p) {
		delete static_cast<std::function<void(const std::string &)> *>(p);
	});
	g_signal_connect(w, "changed", G_CALLBACK(+[](GtkEditable *ed, gpointer) {
		auto *f = static_cast<std::function<void(const std::string &)> *>(
				g_object_get_data(G_OBJECT(ed), "wb-on"));
		(*f)(gtk_editable_get_text(ed));
	}), nullptr);
	return w;
}

/* Earlier / later among its siblings; into the submenu above (at its
 * end); out of its submenu (after it). */
void move_open(Ui *ui, int how) {
	MenuState *st = ui->menu.get();
	if (!st->open || st->open->empty())
		return;
	Path p = *st->open;
	Path parent(p.begin(), p.end() - 1);
	size_t i = p.back();
	wb::MenuEntry menu = current(st);
	const wb::MenuEntry *par = entry_at(menu, parent);
	const wb::MenuEntry *e = entry_at(menu, p);
	if (par == nullptr || e == nullptr)
		return;
	Step s{Step::Op::Move, p, parent, 0, *e, entry_text(*e, *st->doc), ""};
	Path now;
	if (how == -1 && i > 0) {           /* earlier */
		s.index = i - 1;
		now = parent;
		now.push_back(i - 1);
	} else if (how == 1 && i + 1 < par->children.size()) {  /* later */
		s.index = i + 1;
		now = parent;
		now.push_back(i + 1);
	} else if (how == 2 && i > 0 && par->children[i - 1].kind == K::Submenu) {  /* into */
		s.to = parent;
		s.to.push_back(i - 1);
		s.index = par->children[i - 1].children.size();
		now = s.to;
		now.push_back(s.index);
	} else if (how == 3 && !parent.empty()) {  /* out */
		s.to = Path(parent.begin(), parent.end() - 1);
		s.index = parent.back() + 1;
		now = s.to;
		now.push_back(s.index);
	} else {
		return;
	}
	s.after = std::string(_("moved to ")) + path_text(now);
	st->open = now;
	step(ui, s);
	render(ui);
}

void on_remove(GtkButton *, gpointer d) {
	Ui *ui = static_cast<Ui *>(d);
	MenuState *st = ui->menu.get();
	if (!st->open)
		return;
	wb::MenuEntry menu = current(st);
	const wb::MenuEntry *e = entry_at(menu, *st->open);
	if (e == nullptr)
		return;
	Step s{Step::Op::Remove, *st->open, {}, 0, *e, entry_text(*e, *st->doc), _("removed")};
	st->open.reset();
	step(ui, s);
	render(ui);
}

GtkWidget *build_editor(Ui *ui, const wb::MenuEntry &e, const Path &p) {
	MenuState *st = ui->menu.get();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "wb-shortcut-editor");
	gtk_widget_add_css_class(box, "wb-menu-editor");
	GtkSizeGroup *labels = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	g_object_set_data_full(G_OBJECT(box), "wb-labels", labels, g_object_unref);

	if (e.kind != K::Reference) {
		bool needs = e.kind == K::Submenu;
		GtkWidget *label = entry_field(e.label, e.kind == K::Separator ? _("None: a plain divider")
				: needs ? _("A submenu needs a label") : nullptr, 28, false,
				[ui, needs](const std::string &v) {
			/* A submenu with no label (and no entries yet) would read as one
			 * defined elsewhere: an empty label isn't applied to it. */
			if (needs && v.empty())
				return;
			edit_open(ui, [&v](wb::MenuEntry &x) { x.label = v; });
		});
		g_object_set_data(G_OBJECT(box), "wb-label", label);
		gtk_box_append(GTK_BOX(box), field(labels, e.kind == K::Separator ? _("Title")
				: _("Label"), label));
	}
	if (e.kind != K::Separator) {
		GtkWidget *icons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *preview = icon_of(e.icon);
		GtkWidget *icon = entry_field(e.icon, _("An icon's name, or a file"), 28, true,
				[ui, preview](const std::string &v) {
			if (v.empty())
				gtk_image_clear(GTK_IMAGE(preview));
			else if (v[0] == '/')
				gtk_image_set_from_file(GTK_IMAGE(preview), v.c_str());
			else
				gtk_image_set_from_icon_name(GTK_IMAGE(preview), v.c_str());
			edit_open(ui, [&v](wb::MenuEntry &x) { x.icon = v; });
		});
		g_object_set_data(G_OBJECT(box), "wb-icon", icon);
		gtk_box_append(GTK_BOX(icons), preview);
		gtk_box_append(GTK_BOX(icons), icon);
		gtk_box_append(GTK_BOX(box), field(labels, _("Icon"), icons));
	}
	switch (e.kind) {
	case K::Item: {
		wb::BoundAction a = e.actions.empty() ? wb::BoundAction{"Execute", {}, false} : e.actions[0];
		GtkWidget *ae = action_editor_new(labels, a, [ui](const wb::BoundAction &now) {
			edit_open(ui, [&now](wb::MenuEntry &x) {
				if (x.actions.empty())
					x.actions.push_back(now);
				else
					x.actions[0] = now;
			});
		});
		g_object_set_data(G_OBJECT(box), "wb-action", ae);
		gtk_box_append(GTK_BOX(box), ae);
		if (e.actions.size() > 1) {
			std::vector<wb::BoundAction> rest(e.actions.begin() + 1, e.actions.end());
			GtkWidget *also = gtk_label_new((std::string(_("Then also: ")) +
					wb::describe_actions(rest)).c_str());
			gtk_label_set_xalign(GTK_LABEL(also), 0);
			gtk_widget_add_css_class(also, "dim-label");
			gtk_box_append(GTK_BOX(box), also);
		}
		break;
	}
	case K::Pipe: {
		GtkWidget *cmd = entry_field(e.execute, _("A command that prints the menu"), 40, true,
				[ui](const std::string &v) {
			edit_open(ui, [&v](wb::MenuEntry &x) { x.execute = v; });
		});
		g_object_set_data(G_OBJECT(box), "wb-command", cmd);
		gtk_box_append(GTK_BOX(box), field(labels, _("Command"), cmd));
		break;
	}
	case K::Reference: {
		std::vector<std::string> ids;
		for (const std::string &id : st->doc->menu_ids())
			if (id != st->id)
				ids.push_back(id);
		GtkStringList *model = gtk_string_list_new(nullptr);
		guint sel = 0;
		for (size_t i = 0; i < ids.size(); ++i) {
			auto m = st->doc->menu(ids[i]);
			gtk_string_list_append(model, (m && !m->label.empty() ? m->label : ids[i]).c_str());
			if (ids[i] == e.id)
				sel = static_cast<guint>(i);
		}
		GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(model), nullptr);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
		auto *list = new std::vector<std::string>(ids);
		g_object_set_data_full(G_OBJECT(dd), "wb-ids", list, [](gpointer p) {
			delete static_cast<std::vector<std::string> *>(p);
		});
		g_signal_connect(dd, "notify::selected", G_CALLBACK(+[](GObject *o, GParamSpec *,
				gpointer d) {
			auto *ids = static_cast<std::vector<std::string> *>(g_object_get_data(o, "wb-ids"));
			guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
			if (i < ids->size()) {
				std::string id = (*ids)[i];
				edit_open(static_cast<Ui *>(d), [&id](wb::MenuEntry &x) { x.id = id; });
			}
		}), ui);
		gtk_box_append(GTK_BOX(box), field(labels, _("Shows"), dd));
		break;
	}
	default:
		break;
	}
	if (!e.kept.empty()) {
		std::string kept;
		for (const std::string &k : e.kept)
			kept += (kept.empty() ? "" : ", ") + k;
		GtkWidget *k = gtk_label_new((std::string(_("Also kept, as written in menu.xml: ")) +
				kept).c_str());
		gtk_label_set_xalign(GTK_LABEL(k), 0);
		gtk_widget_add_css_class(k, "dim-label");
		gtk_box_append(GTK_BOX(box), k);
	}

	/* Remove on the left; where it goes, and Done, on the right. */
	wb::MenuEntry menu = current(st);
	Path parent(p.begin(), p.end() - 1);
	const wb::MenuEntry *par = entry_at(menu, parent);
	size_t i = p.back();
	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(foot, 4);
	GtkWidget *remove = gtk_button_new_with_label(_("Remove"));
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_add_css_class(remove, "wb-remove");
	g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), ui);
	gtk_box_append(GTK_BOX(foot), remove);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	struct Move { const char *icon, *tip; int how; bool can; };
	bool into = i > 0 && par != nullptr && par->children[i - 1].kind == K::Submenu;
	for (const Move &m : {Move{"go-up-symbolic", _("Earlier"), -1, i > 0},
			Move{"go-down-symbolic", _("Later"), 1, par != nullptr && i + 1 < par->children.size()},
			Move{"go-next-symbolic", _("Into the submenu above"), 2, into},
			Move{"go-previous-symbolic", _("Out of this submenu"), 3, !parent.empty()}}) {
		GtkWidget *b = gtk_button_new_from_icon_name(m.icon);
		gtk_widget_add_css_class(b, "flat");
		gtk_widget_set_tooltip_text(b, m.tip);
		gtk_widget_set_sensitive(b, m.can);
		g_object_set_data(G_OBJECT(b), "wb-how", GINT_TO_POINTER(m.how + 10));
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
			move_open(static_cast<Ui *>(d),
					GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wb-how")) - 10);
		}), ui);
		gtk_box_append(GTK_BOX(foot), b);
	}
	GtkWidget *done = gtk_button_new_with_label(_("Done"));
	g_signal_connect(done, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		Ui *ui = static_cast<Ui *>(d);
		ui->menu->open.reset();
		render(ui);
	}), ui);
	gtk_box_append(GTK_BOX(foot), done);
	gtk_box_append(GTK_BOX(box), foot);
	return box;
}

/* ---- The tree ---- */

void add_rows(Ui *ui, GtkWidget *box, const wb::MenuEntry &menu, Path at, int depth) {
	MenuState *st = ui->menu.get();
	for (size_t i = 0; i < menu.children.size(); ++i) {
		const wb::MenuEntry &e = menu.children[i];
		Path p = at;
		p.push_back(i);
		bool open = st->open && *st->open == p;
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_widget_set_margin_start(row, depth * 26);
		if (e.kind == K::Separator) {
			GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
			gtk_widget_set_valign(line, GTK_ALIGN_CENTER);
			gtk_widget_set_hexpand(line, TRUE);
			if (!e.label.empty()) {
				GtkWidget *t = gtk_label_new(e.label.c_str());
				gtk_widget_add_css_class(t, "wb-menu-title");
				gtk_box_append(GTK_BOX(row), t);
			}
			gtk_box_append(GTK_BOX(row), line);
		} else {
			gtk_box_append(GTK_BOX(row), icon_of(e.icon));
			std::string label = e.label;
			if (label.empty() && e.kind == K::Reference) {
				auto t = st->doc->menu(e.id);
				label = t && !t->label.empty() ? t->label : e.id;
			}
			GtkWidget *l = gtk_label_new(label.c_str());
			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_widget_add_css_class(l, "wb-menu-label");
			gtk_box_append(GTK_BOX(row), l);
			if (e.kind == K::Submenu || e.kind == K::Reference || e.kind == K::Pipe ||
					e.kind == K::Applications) {
				GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
				gtk_widget_add_css_class(arrow, "dim-label");
				gtk_box_append(GTK_BOX(row), arrow);
			}
			GtkWidget *d = gtk_label_new(does_text(e, *st->doc).c_str());
			gtk_label_set_xalign(GTK_LABEL(d), 1);
			gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_END);
			gtk_widget_set_hexpand(d, TRUE);
			gtk_widget_add_css_class(d, "dim-label");
			gtk_box_append(GTK_BOX(row), d);
		}
		GtkWidget *button = gtk_button_new();
		gtk_button_set_child(GTK_BUTTON(button), row);
		gtk_widget_add_css_class(button, "flat");
		gtk_widget_add_css_class(button, "wb-shortcut-row");
		gtk_widget_add_css_class(button, "wb-menu-row");
		if (open)
			gtk_widget_add_css_class(button, "wb-menu-row-open");
		auto *pp = new Path(p);
		g_object_set_data_full(G_OBJECT(button), "wb-path", pp, [](gpointer x) {
			delete static_cast<Path *>(x);
		});
		g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
			Ui *ui = static_cast<Ui *>(d);
			Path p = *static_cast<Path *>(g_object_get_data(G_OBJECT(b), "wb-path"));
			MenuState *st = ui->menu.get();
			if (st->open && *st->open == p)
				st->open.reset();
			else
				st->open = p;
			render(ui);
		}), ui);
		gtk_box_append(GTK_BOX(box), button);
		if (open) {
			GtkWidget *ed = build_editor(ui, e, p);
			gtk_widget_set_margin_start(ed, depth * 26 + 6);
			st->editor = ed;
			gtk_box_append(GTK_BOX(box), ed);
		}
		if (e.kind == K::Submenu)
			add_rows(ui, box, e, p, depth + 1);
	}
}

void render(Ui *ui) {
	MenuState *st = ui->menu.get();
	if (st == nullptr || st->tree == nullptr)
		return;
	while (GtkWidget *c = gtk_widget_get_first_child(st->tree))
		gtk_box_remove(GTK_BOX(st->tree), c);
	st->editor = nullptr;
	gtk_widget_set_visible(st->undo, !st->steps.empty());
	if (!st->doc) {
		GtkWidget *l = gtk_label_new(st->problem.c_str());
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_widget_add_css_class(l, "wb-rule-error");
		gtk_box_append(GTK_BOX(st->tree), l);
		return;
	}
	auto menu = st->doc->menu(st->id);
	if (!menu) {
		GtkWidget *l = gtk_label_new(_("menu.xml has no root menu (id “root-menu”)."));
		gtk_widget_add_css_class(l, "dim-label");
		gtk_box_append(GTK_BOX(st->tree), l);
		return;
	}
	add_rows(ui, st->tree, *menu, {}, 0);
}

void changed(Ui *ui) {
	MenuState *st = ui->menu.get();
	gtk_widget_set_visible(st->undo, !st->steps.empty());
	refresh_save_state(ui);
}

/* Add: a new entry at the end of the menu, its editor open. */
void add_entry(Ui *ui, K kind) {
	MenuState *st = ui->menu.get();
	if (!st->doc)
		return;
	wb::MenuEntry e;
	e.kind = kind;
	switch (kind) {
	case K::Item:
		e.label = _("New Program");
		e.actions = {{"Execute", {}, false}};
		break;
	case K::Submenu:
		e.label = _("New Submenu");
		e.id = fresh_id(st, e.label);
		break;
	case K::Applications:
		e.label = _("Applications");
		e.icon = "applications-other";
		e.id = fresh_id(st, "applications");
		break;
	case K::Pipe:
		e.label = _("New Pipe Menu");
		e.id = fresh_id(st, "pipe");
		break;
	default:
		break;
	}
	size_t n = current(st).children.size();
	Step s{Step::Op::Insert, {}, {}, n, e, _("none"), entry_text(e, *st->doc)};
	st->open = Path{n};
	step(ui, s);
	render(ui);
}

}  // namespace

GtkWidget *build_menu_block(Ui *ui) {
	ui->menu = std::make_shared<MenuState>();
	MenuState *st = ui->menu.get();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	st->tree = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(st->tree, "wb-menu-tree");
	gtk_box_append(GTK_BOX(box), st->tree);

	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_top(foot, 6);
	GtkWidget *add = gtk_menu_button_new();
	GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(inside), gtk_image_new_from_icon_name("list-add-symbolic"));
	gtk_box_append(GTK_BOX(inside), gtk_label_new(_("Add")));
	gtk_menu_button_set_child(GTK_MENU_BUTTON(add), inside);
	gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(add), FALSE);
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	struct Kind { const char *label; K kind; };
	for (const Kind &k : {Kind{_("A Program"), K::Item}, Kind{_("A Separator"), K::Separator},
			Kind{_("A Submenu"), K::Submenu}, Kind{_("The Applications Menu"), K::Applications},
			Kind{_("A Pipe Menu"), K::Pipe}}) {
		GtkWidget *b = gtk_button_new_with_label(k.label);
		gtk_widget_add_css_class(b, "flat");
		gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
		g_object_set_data(G_OBJECT(b), "wb-kind", GINT_TO_POINTER(static_cast<int>(k.kind) + 1));
		g_object_set_data(G_OBJECT(b), "wb-pop", pop);
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
			gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(btn), "wb-pop")));
			add_entry(static_cast<Ui *>(d), static_cast<K>(
					GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "wb-kind")) - 1));
		}), ui);
		gtk_box_append(GTK_BOX(list), b);
	}
	gtk_popover_set_child(GTK_POPOVER(pop), list);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(add), pop);
	gtk_box_append(GTK_BOX(foot), add);
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(gap, TRUE);
	gtk_box_append(GTK_BOX(foot), gap);
	st->undo = gtk_button_new_with_label(_("Undo"));
	gtk_widget_add_css_class(st->undo, "flat");
	gtk_widget_set_tooltip_text(st->undo, _("Take back the last change to the menu"));
	gtk_widget_set_visible(st->undo, FALSE);
	g_signal_connect(st->undo, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
		Ui *ui = static_cast<Ui *>(d);
		MenuState *st = ui->menu.get();
		if (st->steps.empty())
			return;
		st->steps.pop_back();
		st->open.reset();
		rebuild(st);
		render(ui);
		changed(ui);
	}), ui);
	gtk_box_append(GTK_BOX(foot), st->undo);
	gtk_box_append(GTK_BOX(box), foot);
	return box;
}

void populate_menu(Ui *ui) {
	MenuState *st = ui->menu.get();
	if (st == nullptr)
		return;
	st->path = user_menu_path();
	std::string from = !st->path.empty() && fs::exists(st->path) ? st->path : default_menu_path();
	st->loaded = read_text(from);
	st->steps.clear();
	st->open.reset();
	std::string err;
	st->doc = wb::MenuDocument::from_string(st->loaded, &err);
	st->problem = st->doc ? "" : from + ": " + err + ". " + _("Fix it, then reopen wbconf.");
	render(ui);
}

bool apply_menu(Ui *ui, std::string *error) {
	MenuState *st = ui->menu.get();
	if (st == nullptr || st->steps.empty() || !st->doc)
		return true;
	std::error_code ec;
	fs::create_directories(fs::path(st->path).parent_path(), ec);
	if (!st->doc->save_file(st->path, error))
		return false;
	st->loaded = st->doc->to_string();
	st->steps.clear();
	render(ui);
	return true;
}

void menu_changes(Ui *ui, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out) {
	MenuState *st = ui->menu.get();
	if (st == nullptr)
		return;
	for (const Step &s : st->steps)
		out.push_back({{where, s.before, s.after}, target});
}

void menu_dump(Ui *ui) {
	MenuState *st = ui->menu.get();
	fprintf(stderr, "menu-dump:%zu\n", st ? st->steps.size() : 0);
	if (st == nullptr || !st->doc)
		return;
	auto menu = st->doc->menu(st->id);
	if (!menu)
		return;
	std::function<void(const wb::MenuEntry &, Path)> walk = [&](const wb::MenuEntry &m, Path at) {
		for (size_t i = 0; i < m.children.size(); ++i) {
			Path p = at;
			p.push_back(i);
			const wb::MenuEntry &e = m.children[i];
			fprintf(stderr, "menu|%s|%s|%s|%s\n", path_text(p).c_str(), e.label.c_str(),
					e.icon.c_str(), does_text(e, *st->doc).c_str());
			if (e.kind == K::Submenu)
				walk(e, p);
		}
	};
	walk(*menu, {});
}

/* Test steps (PATH: "3" the third entry, "4.1" the fourth's first):
 *   menu-open:PATH   menu-set:PATH=label|icon|command=VALUE
 *   menu-action:PATH=Name   menu-option:PATH=name=VALUE
 *   menu-remove:PATH   menu-earlier|later|into|out:PATH
 *   menu-add:item|separator|submenu|applications|pipe   menu-undo
 */
void menu_test_step(Ui *ui, const std::string &stp) {
	MenuState *st = ui->menu.get();
	if (st == nullptr)
		return;
	size_t colon = stp.find(':');
	std::string verb = stp.substr(0, colon);
	std::string arg = colon == std::string::npos ? "" : stp.substr(colon + 1);
	if (verb == "menu-undo") {
		g_signal_emit_by_name(st->undo, "clicked");
		return;
	}
	if (verb == "menu-add") {
		K k = arg == "separator" ? K::Separator : arg == "submenu" ? K::Submenu
				: arg == "applications" ? K::Applications : arg == "pipe" ? K::Pipe : K::Item;
		add_entry(ui, k);
		return;
	}
	size_t eq = arg.find('=');
	Path p = parse_path(arg.substr(0, eq));
	std::string rest = eq == std::string::npos ? "" : arg.substr(eq + 1);
	if (!st->open || *st->open != p) {
		st->open = p;
		render(ui);
	}
	if (st->editor == nullptr)
		return;
	if (verb == "menu-set") {
		size_t eq2 = rest.find('=');
		std::string f = rest.substr(0, eq2), v = rest.substr(eq2 + 1);
		const char *key = f == "label" ? "wb-label" : f == "icon" ? "wb-icon" : "wb-command";
		if (auto *w = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(st->editor), key)))
			gtk_editable_set_text(GTK_EDITABLE(w), v.c_str());
	} else if (verb == "menu-action" || verb == "menu-option") {
		auto *ae = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(st->editor), "wb-action"));
		if (ae == nullptr)
			return;
		if (verb == "menu-action") {
			action_editor_select(ae, rest);
		} else {
			size_t eq2 = rest.find('=');
			action_editor_set_option(ae, rest.substr(0, eq2), rest.substr(eq2 + 1));
		}
	} else if (verb == "menu-remove") {
		for (GtkWidget *c = gtk_widget_get_last_child(st->editor); c != nullptr;
				c = gtk_widget_get_prev_sibling(c))
			for (GtkWidget *b = gtk_widget_get_first_child(c); b != nullptr;
					b = gtk_widget_get_next_sibling(b))
				if (GTK_IS_BUTTON(b) && gtk_widget_has_css_class(b, "wb-remove")) {
					g_signal_emit_by_name(b, "clicked");
					return;
				}
	} else if (verb == "menu-earlier") {
		move_open(ui, -1);
	} else if (verb == "menu-later") {
		move_open(ui, 1);
	} else if (verb == "menu-into") {
		move_open(ui, 2);
	} else if (verb == "menu-out") {
		move_open(ui, 3);
	}
}

}  // namespace wbconf
