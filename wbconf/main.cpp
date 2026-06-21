/*
 * wbconf — a small GTK4 settings GUI for waybox, modelled after Openbox's
 * obconf (in scope and in visual layout: bold section headers over indented
 * content, a scrolling theme list, obconf-style tabs). It edits the user's
 * rc.xml in place (preserving keybindings, application rules and comments)
 * through wb::RcDocument, then asks a running waybox to reload (SIGUSR2). Only
 * the settings the compositor honours are exposed, plus the waybox extensions
 * (titlebar / menu / switcher).
 *
 * All user-visible strings are wrapped in _() for translation, consistent with
 * the rest of the project's gettext catalogue (text domain "waybox").
 */
#include <gtk/gtk.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>
#include <libintl.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/wbconf.hpp"
#include "waybox/theme.hpp"
#include "waybox/style.hpp"
#include "waybox/render.hpp"

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "waybox"
#endif
#define _(s) gettext(s)

namespace {

namespace fs = std::filesystem;

/* A blank rc.xml used only when the user has no config and none is installed. */
const char *kSkeletonRc =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"</openbox_config>\n";

std::string home_dir() {
	if (const char *h = getenv("HOME"); h && h[0])
		return h;
	return {};
}

/* Where wbconf writes: $XDG_CONFIG_HOME/waybox/rc.xml, else ~/.config/...  */
std::string user_rc_path() {
	if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
		return (fs::path(xdg) / "waybox" / "rc.xml").string();
	std::string h = home_dir();
	if (!h.empty())
		return (fs::path(h) / ".config" / "waybox" / "rc.xml").string();
	return {};
}

/* The system default, used to seed a brand-new user config. */
std::string system_rc_path() {
#ifdef WB_SYSCONFDIR
	return (fs::path(WB_SYSCONFDIR) / "xdg" / "waybox" / "rc.xml").string();
#else
	return {};
#endif
}

/* Load the document to edit: the user's file if present, else the system
 * default (so the user inherits its settings), else a blank skeleton. */
wb::RcDocument load_document() {
	std::error_code ec;
	std::string up = user_rc_path();
	if (!up.empty() && fs::exists(up, ec))
		if (auto d = wb::RcDocument::load_file(up))
			return std::move(*d);
	std::string sp = system_rc_path();
	if (!sp.empty() && fs::exists(sp, ec))
		if (auto d = wb::RcDocument::load_file(sp))
			return std::move(*d);
	return *wb::RcDocument::from_string(kSkeletonRc);
}

/* Ask every waybox process owned by this user to reconfigure (SIGUSR2), the
 * same signal Openbox uses. Returns the number signalled. */
int signal_waybox_reload() {
	int signalled = 0;
	uid_t me = getuid();
	DIR *proc = opendir("/proc");
	if (proc == nullptr)
		return 0;
	struct dirent *e;
	while ((e = readdir(proc)) != nullptr) {
		char *end = nullptr;
		long pid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0' || pid <= 0)
			continue;
		std::string comm_path = std::string("/proc/") + e->d_name + "/comm";
		std::ifstream comm(comm_path);
		std::string name;
		if (!std::getline(comm, name) || name != "waybox")
			continue;
		struct stat st {};
		std::string dir = std::string("/proc/") + e->d_name;
		if (stat(dir.c_str(), &st) != 0 || st.st_uid != me)
			continue;
		if (kill(static_cast<pid_t>(pid), SIGUSR2) == 0)
			++signalled;
	}
	closedir(proc);
	return signalled;
}

/* ---- A lightweight font picker: family + size + bold ----------------- */

struct Ui;

struct FontPicker {
	GtkDropDown *family = nullptr;
	GtkSpinButton *size = nullptr;
	GtkToggleButton *bold = nullptr;
	GtkToggleButton *link = nullptr;  /* padlock: make this row the master */
	std::vector<std::string> families;  /* parallel to the dropdown model */
	/* Stable per-picker context so signal handlers know which row fired. */
	Ui *ui = nullptr;
	int index = -1;
};

/* ---- Widget set, bound to a WayboxSettings on save ------------------- */

struct Ui {
	GtkWindow *window = nullptr;
	GtkLabel *status = nullptr;

	/* Appearance */
	GtkListBox *theme_list = nullptr;        /* obconf-style scrolling list */
	std::vector<std::string> theme_names;    /* parallel to the list rows */
	GtkSpinButton *pad_y = nullptr;
	GtkSpinButton *button_size = nullptr;
	GtkSpinButton *resize_grab = nullptr;

	/* Windows */
	GtkDropDown *placement = nullptr;  /* Smart / Center / UnderMouse */

	/* Margins */
	GtkSpinButton *margin_top = nullptr;
	GtkSpinButton *margin_bottom = nullptr;
	GtkSpinButton *margin_left = nullptr;
	GtkSpinButton *margin_right = nullptr;

	/* Menu */
	GtkEntry *menu_source = nullptr;
	GtkDropDown *submenu_open = nullptr;  /* hover / click */
	GtkSpinButton *hover_delay = nullptr;
	GtkSwitch *menu_wrap = nullptr;
	GtkSwitch *menu_icons = nullptr;

	/* Switcher */
	GtkDropDown *switcher_order = nullptr;  /* mru / stacking / spatial */
	GtkSwitch *switcher_osd = nullptr;
	GtkSwitch *switcher_wrap = nullptr;

	/* Fonts: a lightweight custom picker per place (family dropdown + size spin
	 * + bold toggle). We avoid GtkFontDialogButton because GTK's font chooser
	 * loads its list asynchronously and only highlights/scrolls to the current
	 * font after a multi-second delay; the custom picker is instant and always
	 * shows the current selection. */
	FontPicker font_active;
	FontPicker font_inactive;
	FontPicker font_menu_header;
	FontPicker font_menu_item;
	FontPicker font_osd;

	/* When one font row's padlock is closed it becomes the master: every other
	 * row greys out and live-mirrors its family/size/bold. -1 = no link. */
	int link_master = -1;

	/* Live theme preview (redrawn when the theme/fonts change). */
	GtkWidget *preview = nullptr;
	GtkStack *stack = nullptr;  /* vertical-tab pages; for "reset this page" */

	/* The settings as loaded, so Save only writes fonts the user changed
	 * (the font buttons are seeded with a default so the picker pre-selects a
	 * font, but we must not persist that default unless it was edited). */
	wb::WayboxSettings original;
};

const char *const kPlacement[] = {"Smart", "Center", "UnderMouse", nullptr};
const char *const kSubmenu[] = {"hover", "click", nullptr};
const char *const kOrder[] = {"mru", "stacking", "spatial", nullptr};

int index_of(const char *const *list, const std::string &value, int fallback) {
	for (int i = 0; list[i] != nullptr; ++i)
		if (value == list[i])
			return i;
	return fallback;
}

/* ---- obconf-style layout helpers ------------------------------------- */

/* A notebook page: a vertical box with obconf's outer padding. */
GtkWidget *make_page() {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(page, 12);
	gtk_widget_set_margin_bottom(page, 12);
	gtk_widget_set_margin_start(page, 12);
	gtk_widget_set_margin_end(page, 12);
	return page;
}

/* A bold section header followed by an indented content box (obconf's idiom of
 * "<span weight=\"bold\">Title</span>" over a 12px-indented VBox). Returns the
 * content box to which callers append their controls. */
GtkWidget *section(GtkWidget *page, const char *title) {
	GtkWidget *header = gtk_label_new(nullptr);
	char *markup = g_markup_printf_escaped("<span weight=\"bold\">%s</span>",
			title);
	gtk_label_set_markup(GTK_LABEL(header), markup);
	g_free(markup);
	gtk_widget_set_halign(header, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), header);

	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(content, 12);
	gtk_box_append(GTK_BOX(page), content);
	return content;
}

/* A grid for label/control rows, appended to a section's content box. */
GtkWidget *section_grid(GtkWidget *content) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_box_append(GTK_BOX(content), grid);
	return grid;
}

void grid_label(GtkWidget *grid, int row, const char *text) {
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
}

GtkWidget *grid_control(GtkWidget *grid, int row, GtkWidget *w) {
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
	return w;
}

GtkSpinButton *add_spin(GtkWidget *grid, int row, const char *label,
		int min, int max) {
	grid_label(grid, row, label);
	GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);
	grid_control(grid, row, spin);
	return GTK_SPIN_BUTTON(spin);
}

GtkSwitch *add_switch(GtkWidget *grid, int row, const char *label) {
	grid_label(grid, row, label);
	GtkWidget *sw = gtk_switch_new();
	gtk_widget_set_halign(sw, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), sw, 1, row, 1, 1);
	return GTK_SWITCH(sw);
}

GtkDropDown *add_dropdown(GtkWidget *grid, int row, const char *label,
		const char *const *items) {
	grid_label(grid, row, label);
	GtkWidget *dd = gtk_drop_down_new_from_strings(items);
	grid_control(grid, row, dd);
	return GTK_DROP_DOWN(dd);
}

/* ---- Fonts + live theme preview -------------------------------------- */

/* The installed font families, sorted and de-duplicated. Uses the default
 * Cairo/Pango font map, so no widget is needed. */
const std::vector<std::string> &font_families() {
	static const std::vector<std::string> families = [] {
		std::vector<std::string> out;
		PangoFontMap *fm = pango_cairo_font_map_get_default();
		PangoFontFamily **fams = nullptr;
		int n = 0;
		pango_font_map_list_families(fm, &fams, &n);
		out.reserve(n);
		for (int i = 0; i < n; i++) {
			const char *name = pango_font_family_get_name(fams[i]);
			if (name != nullptr)
				out.emplace_back(name);
		}
		g_free(fams);
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}();
	return families;
}

/* List-item factory callbacks that render each family name in its own font, so
 * the drop-down (both the list and the selected button) previews the family. */
void family_item_setup(GtkSignalListItemFactory *, GtkListItem *item,
		gpointer) {
	GtkWidget *label = gtk_label_new(nullptr);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(item, label);
}

void family_item_bind(GtkSignalListItemFactory *, GtkListItem *item,
		gpointer) {
	GtkWidget *label = gtk_list_item_get_child(item);
	auto *obj = static_cast<GtkStringObject *>(gtk_list_item_get_item(item));
	if (label == nullptr || obj == nullptr)
		return;
	const char *family = gtk_string_object_get_string(obj);
	gtk_label_set_text(GTK_LABEL(label), family);
	/* Render the name in its own family at a readable size. */
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_family_new(family));
	pango_attr_list_insert(attrs, pango_attr_size_new(12 * PANGO_SCALE));
	gtk_label_set_attributes(GTK_LABEL(label), attrs);
	pango_attr_list_unref(attrs);
}

/* Font-row signal handlers (defined after the Ui helpers below). */
void on_font_changed(FontPicker *p);
void on_link_toggled(GtkToggleButton *btn, gpointer data);

/* A custom, instant font picker: a padlock that links this row to the others, a
 * family dropdown (each item rendered in its own font), a size spin button and
 * a bold toggle, laid out in one row. */
void add_font(GtkWidget *grid, int row, const char *label, FontPicker *out,
		Ui *ui, int index) {
	grid_label(grid, row, label);
	out->ui = ui;
	out->index = index;

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_hexpand(box, TRUE);

	/* Padlock: closing it links every other row to this one. */
	GtkWidget *link = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(link), "changes-allow-symbolic");
	gtk_widget_set_tooltip_text(link,
			_("Link the other fonts to this one"));
	gtk_widget_add_css_class(link, "flat");
	out->link = GTK_TOGGLE_BUTTON(link);
	gtk_box_append(GTK_BOX(box), link);

	out->families = font_families();
	std::vector<const char *> items;
	items.reserve(out->families.size() + 1);
	for (const std::string &f : out->families)
		items.push_back(f.c_str());
	items.push_back(nullptr);
	GtkWidget *fam = gtk_drop_down_new_from_strings(items.data());
	gtk_widget_set_hexpand(fam, TRUE);
	/* Make the long list searchable by typing. */
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(fam), TRUE);
	/* Render each family in its own font (button + list). */
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(family_item_setup), nullptr);
	g_signal_connect(factory, "bind", G_CALLBACK(family_item_bind), nullptr);
	gtk_drop_down_set_factory(GTK_DROP_DOWN(fam), factory);
	g_object_unref(factory);
	out->family = GTK_DROP_DOWN(fam);
	gtk_box_append(GTK_BOX(box), fam);

	GtkWidget *size = gtk_spin_button_new_with_range(4, 96, 1);
	out->size = GTK_SPIN_BUTTON(size);
	gtk_box_append(GTK_BOX(box), size);

	GtkWidget *bold = gtk_toggle_button_new_with_label(_("B"));
	gtk_widget_set_tooltip_text(bold, _("Bold"));
	out->bold = GTK_TOGGLE_BUTTON(bold);
	gtk_box_append(GTK_BOX(box), bold);

	gtk_grid_attach(GTK_GRID(grid), box, 1, row, 1, 1);

	g_signal_connect_swapped(fam, "notify::selected",
			G_CALLBACK(on_font_changed), out);
	g_signal_connect_swapped(size, "value-changed",
			G_CALLBACK(on_font_changed), out);
	g_signal_connect_swapped(bold, "toggled",
			G_CALLBACK(on_font_changed), out);
	g_signal_connect(link, "toggled", G_CALLBACK(on_link_toggled), out);
}

/* Read a picker as a Pango-style "Family [Bold] Size" string. */
std::optional<std::string> font_value(const FontPicker *p) {
	guint idx = gtk_drop_down_get_selected(p->family);
	if (idx == GTK_INVALID_LIST_POSITION || idx >= p->families.size())
		return std::nullopt;
	std::string out = p->families[idx];
	if (gtk_toggle_button_get_active(p->bold))
		out += " Bold";
	out += " " + std::to_string(gtk_spin_button_get_value_as_int(p->size));
	return out;
}

/* Set a picker from a Pango-style "Family [Bold] Size" string. */
void font_set(FontPicker *p, const std::optional<std::string> &v) {
	if (!v)
		return;
	PangoFontDescription *d = pango_font_description_from_string(v->c_str());
	const char *fam = pango_font_description_get_family(d);
	std::string family = fam ? fam : "Sans";
	int size = pango_font_description_get_size(d);
	int size_pt = size > 0 ? size / PANGO_SCALE : 10;
	bool bold = pango_font_description_get_weight(d) >= PANGO_WEIGHT_BOLD;
	pango_font_description_free(d);

	guint sel = 0;
	for (guint i = 0; i < p->families.size(); i++) {
		if (p->families[i] == family) { sel = i; break; }
	}
	gtk_drop_down_set_selected(p->family, sel);
	gtk_spin_button_set_value(p->size, size_pt);
	gtk_toggle_button_set_active(p->bold, bold);
}

/* Collect the current settings from all widgets (forward declaration so the
 * preview can render the live, unsaved state). */
struct Ui;
wb::WayboxSettings collect(Ui *ui);
void set_status(Ui *ui, const std::string &text);

void to_cairo(cairo_t *cr, const wb::Color &c) {
	cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
			c.a / 255.0);
}

/* Draw one of the three glyph shapes (iconify, maximize, close) centred in a
 * square, in the given colour. */
void preview_glyph(cairo_t *cr, double cx, double cy, double s, int kind,
		const wb::Color &fg) {
	to_cairo(cr, fg);
	cairo_set_line_width(cr, 1.5);
	if (kind == 0) {  /* iconify: bottom line */
		cairo_move_to(cr, cx - s, cy + s);
		cairo_line_to(cr, cx + s, cy + s);
	} else if (kind == 1) {  /* maximize: square */
		cairo_rectangle(cr, cx - s, cy - s, 2 * s, 2 * s);
	} else {  /* close: X */
		cairo_move_to(cr, cx - s, cy - s);
		cairo_line_to(cr, cx + s, cy + s);
		cairo_move_to(cr, cx + s, cy - s);
		cairo_line_to(cr, cx - s, cy + s);
	}
	cairo_stroke(cr);
}

/* Draw a single titlebar sample (active or inactive) with the theme's colours
 * and label font, plus the three window buttons. Returns the height drawn. */
int preview_titlebar(cairo_t *cr, const wb::Theme &theme, bool active,
		int x, int y, int w, const char *title) {
	wb::FrameStyle fs = wb::frame_style_from_theme(theme, active);
	int text_h = wb::measure_text("Ag", fs.label.font).height;
	int pad = std::max(theme.padding_y, 3);
	int h = text_h + 2 * pad;
	if (h < 18)
		h = 18;

	/* border + titlebar fill */
	to_cairo(cr, fs.border.color);
	cairo_rectangle(cr, x - 1, y - 1, w + 2, h + 2);
	cairo_fill(cr);
	wb::paint_fill(cr, x, y, w, h, fs.title_bar.fill);

	/* label */
	int ty = y + (h - text_h) / 2;
	wb::paint_text(cr, x + 6, ty, title, fs.label.color, fs.label.font);

	/* three buttons on the right */
	int bs = (h * 2) / 3;
	int by = y + (h - bs) / 2;
	int bx = x + w - 6 - 3 * (bs + 2);
	const wb::StateStyle &btn = fs.button.for_state(wb::WidgetState::Normal);
	for (int i = 0; i < 3; i++) {
		int cx = bx + i * (bs + 2) + bs / 2;
		preview_glyph(cr, cx, by + bs / 2.0, bs * 0.28, i, btn.fg);
	}
	return h;
}

void preview_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
		gpointer data) {
	Ui *ui = static_cast<Ui *>(data);
	wb::WayboxSettings s = collect(ui);

	/* Resolve the theme being previewed (the live selection), with the live
	 * font overrides applied on top. */
	wb::Theme theme = s.theme_name ? wb::load_theme(*s.theme_name)
			: wb::default_theme();
	auto apply_font = [](const std::optional<std::string> &v, wb::FontSpec &f) {
		if (!v)
			return;
		PangoFontDescription *d = pango_font_description_from_string(v->c_str());
		if (const char *fam = pango_font_description_get_family(d))
			f.family = fam;
		int sz = pango_font_description_get_size(d);
		if (sz > 0)
			f.size_pt = pango_font_description_get_size_is_absolute(d)
					? sz / PANGO_SCALE
					: sz / PANGO_SCALE;
		f.bold = pango_font_description_get_weight(d) >= PANGO_WEIGHT_BOLD;
		pango_font_description_free(d);
	};
	apply_font(s.font_active_window, theme.font_active_title);
	apply_font(s.font_inactive_window, theme.font_inactive_title);
	apply_font(s.font_menu_item, theme.font_menu_item);
	apply_font(s.font_osd, theme.font_osd);

	/* Backdrop. */
	cairo_set_source_rgb(cr, 0.18, 0.18, 0.2);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_fill(cr);

	int margin = 12;
	int w = width - 2 * margin;
	int y = margin;
	y += preview_titlebar(cr, theme, true, margin, y, w, _("Active window")) + 4;
	/* a thin client area below the active titlebar */
	cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
	cairo_rectangle(cr, margin, y, w, 26);
	cairo_fill(cr);
	y += 26 + 12;
	y += preview_titlebar(cr, theme, false, margin, y, w, _("Inactive window")) + 4;

	/* A small menu sample. */
	wb::MenuStyle ms = wb::menu_style_from_theme(theme);
	y += 12;
	int mh = wb::measure_text("Ag", ms.item_text.font).height + 6;
	const char *items[] = {_("Terminal"), _("Web Browser"), _("Files")};
	int mw = w / 2;
	for (int i = 0; i < 3; i++) {
		bool hot = (i == 1);
		const wb::StateStyle &st = ms.item.for_state(
				hot ? wb::WidgetState::Hover : wb::WidgetState::Normal);
		wb::paint_fill(cr, margin, y, mw, mh, st.fill);
		wb::paint_text(cr, margin + 8, y + 3, items[i], st.fg,
				ms.item_text.font);
		y += mh;
	}
}

void preview_refresh(Ui *ui) {
	if (ui->preview != nullptr)
		gtk_widget_queue_draw(ui->preview);
}

/* ---- Per-theme mini swatch (the right side of each theme-list row) ----
 * Replicates obconf's feel: a tiny stack with the menu on top (a normal entry
 * with a submenu arrow, a disabled entry and a selected entry), then the active
 * titlebar, then the inactive titlebar, all in miniature using the theme's own
 * colours and fonts. */

void free_std_string(gpointer p) { delete static_cast<std::string *>(p); }

void theme_swatch_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		gpointer) {
	auto *name = static_cast<std::string *>(
			g_object_get_data(G_OBJECT(area), "theme"));
	wb::Theme theme = (name != nullptr && !name->empty())
			? wb::load_theme(*name)
			: wb::default_theme();

	const int x = 1;
	const int w = width - 2;

	/* Backdrop. */
	cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_fill(cr);

	auto tiny = [](wb::FontSpec f) { f.size_pt = 7; f.bold = false; return f; };

	double y = 1;

	/* --- Menu on top: normal (with submenu arrow), disabled, selected. --- */
	wb::MenuStyle ms = wb::menu_style_from_theme(theme);
	wb::FontSpec mfont = tiny(ms.item_text.font);
	int rh = wb::measure_text("Ag", mfont).height + 4;
	struct Row { const char *text; wb::WidgetState st; bool arrow; };
	Row rows[] = {
		{_("Normal"), wb::WidgetState::Normal, true},
		{_("Disabled"), wb::WidgetState::Disabled, false},
		{_("Selected"), wb::WidgetState::Hover, false},
	};
	/* Panel border + fill behind the three rows. */
	to_cairo(cr, ms.panel.border.color);
	cairo_rectangle(cr, x - 0.5, y - 0.5, w + 1, 3 * rh + 1);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);
	wb::paint_fill(cr, x, y, w, 3 * rh, ms.panel.fill);
	for (const Row &r : rows) {
		const wb::StateStyle &st = ms.item.for_state(r.st);
		wb::paint_fill(cr, x, y, w, rh, st.fill);
		wb::paint_text(cr, x + 4, y + 2, r.text, st.fg, mfont);
		if (r.arrow)
			wb::paint_text(cr, x + w - 8, y + 2, "\u203A", st.fg, mfont);
		y += rh;
	}

	y += 3;

	/* --- Active then inactive titlebars in miniature. --- */
	auto titlebar = [&](bool active, const char *title) {
		wb::FrameStyle fs = wb::frame_style_from_theme(theme, active);
		wb::FontSpec lf = tiny(fs.label.font);
		int th = wb::measure_text("Ag", lf).height + 4;
		to_cairo(cr, fs.border.color);
		cairo_rectangle(cr, x - 0.5, y - 0.5, w + 1, th + 1);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
		wb::paint_fill(cr, x, y, w, th, fs.title_bar.fill);
		wb::paint_text(cr, x + 4, y + 2, title, fs.label.color, lf);
		/* three tiny buttons on the right */
		const wb::StateStyle &btn = fs.button.for_state(wb::WidgetState::Normal);
		int bs = th - 4;
		for (int i = 0; i < 3; i++) {
			double bx = x + w - 4 - (i + 1) * (bs + 2) + 2;
			to_cairo(cr, btn.fg);
			cairo_set_line_width(cr, 1.0);
			cairo_rectangle(cr, bx, y + 2, bs, bs);
			cairo_stroke(cr);
		}
		y += th + 2;
	};
	titlebar(true, _("Active"));
	titlebar(false, _("Inactive"));
}

/* All five font pickers, indexed by their row position. */
std::vector<FontPicker *> all_pickers(Ui *ui) {
	return {&ui->font_active, &ui->font_inactive, &ui->font_menu_header,
			&ui->font_menu_item, &ui->font_osd};
}

/* Show a closed padlock on the master row, an open one elsewhere. */
void set_link_icon(FontPicker *p) {
	bool master = p->ui->link_master == p->index;
	gtk_button_set_icon_name(GTK_BUTTON(p->link),
			master ? "changes-prevent-symbolic" : "changes-allow-symbolic");
}

/* Grey out every non-master row's controls while a link is engaged. */
void set_followers_sensitive(Ui *ui) {
	for (FontPicker *p : all_pickers(ui)) {
		bool enabled = ui->link_master < 0 || p->index == ui->link_master;
		gtk_widget_set_sensitive(GTK_WIDGET(p->family), enabled);
		gtk_widget_set_sensitive(GTK_WIDGET(p->size), enabled);
		gtk_widget_set_sensitive(GTK_WIDGET(p->bold), enabled);
		set_link_icon(p);
	}
}

/* Copy the master row's family/size/bold onto every other row. */
void apply_master_to_followers(Ui *ui) {
	if (ui->link_master < 0)
		return;
	std::vector<FontPicker *> ps = all_pickers(ui);
	FontPicker *m = ps[ui->link_master];
	guint fam = gtk_drop_down_get_selected(m->family);
	int sz = gtk_spin_button_get_value_as_int(m->size);
	bool bold = gtk_toggle_button_get_active(m->bold);
	for (FontPicker *p : ps) {
		if (p->index == ui->link_master)
			continue;
		if (fam != GTK_INVALID_LIST_POSITION && fam < p->families.size())
			gtk_drop_down_set_selected(p->family, fam);
		gtk_spin_button_set_value(p->size, sz);
		gtk_toggle_button_set_active(p->bold, bold);
	}
}

/* Any family/size/bold change: if it's the master, push it to the followers;
 * always refresh the live preview. */
void on_font_changed(FontPicker *p) {
	Ui *ui = p->ui;
	if (ui->link_master == p->index)
		apply_master_to_followers(ui);
	preview_refresh(ui);
}

/* Padlock toggled: closing makes this row the master (others grey out and
 * mirror it); opening the master's padlock unlinks everything. */
void on_link_toggled(GtkToggleButton *btn, gpointer data) {
	auto *p = static_cast<FontPicker *>(data);
	Ui *ui = p->ui;
	if (gtk_toggle_button_get_active(btn)) {
		ui->link_master = p->index;
		/* Only one master at a time: pop any other closed padlock. */
		for (FontPicker *q : all_pickers(ui)) {
			if (q->index != p->index &&
					gtk_toggle_button_get_active(q->link)) {
				g_signal_handlers_block_by_func(
						q->link, (gpointer)on_link_toggled, q);
				gtk_toggle_button_set_active(q->link, FALSE);
				g_signal_handlers_unblock_by_func(
						q->link, (gpointer)on_link_toggled, q);
			}
		}
		set_followers_sensitive(ui);
		apply_master_to_followers(ui);
		set_status(ui, _("Linked the other fonts to this one."));
	} else if (ui->link_master == p->index) {
		ui->link_master = -1;
		set_followers_sensitive(ui);
		set_status(ui, _("Unlinked fonts."));
	}
	preview_refresh(ui);
}

void populate(Ui *ui, const wb::WayboxSettings &s) {
	ui->original = s;  /* remember the loaded state (for change detection) */
	/* Theme list: every installed theme, current one guaranteed present and
	 * selected (obconf shows the active theme highlighted in the list). */
	ui->theme_names = wb::installed_theme_names();
	std::string current = s.theme_name.value_or("");
	if (!current.empty() &&
			std::find(ui->theme_names.begin(), ui->theme_names.end(), current) ==
					ui->theme_names.end())
		ui->theme_names.insert(ui->theme_names.begin(), current);
	if (ui->theme_names.empty() && !current.empty())
		ui->theme_names.push_back(current);

	/* Clear any existing rows, then repopulate. */
	GtkListBoxRow *row;
	while ((row = gtk_list_box_get_row_at_index(ui->theme_list, 0)) != nullptr)
		gtk_list_box_remove(ui->theme_list, GTK_WIDGET(row));
	int sel = -1;
	for (size_t i = 0; i < ui->theme_names.size(); ++i) {
		/* obconf-style row: theme name on the left, a tiny live preview of the
		 * theme (menu + active/inactive titlebars) pushed to the right edge. */
		GtkWidget *rowbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_top(rowbox, 4);
		gtk_widget_set_margin_bottom(rowbox, 4);
		gtk_widget_set_margin_start(rowbox, 6);
		gtk_widget_set_margin_end(rowbox, 6);

		GtkWidget *label = gtk_label_new(ui->theme_names[i].c_str());
		gtk_widget_set_halign(label, GTK_ALIGN_START);
		gtk_widget_set_hexpand(label, TRUE);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_box_append(GTK_BOX(rowbox), label);

		GtkWidget *swatch = gtk_drawing_area_new();
		gtk_widget_set_size_request(swatch, 132, 78);
		g_object_set_data_full(G_OBJECT(swatch), "theme",
				new std::string(ui->theme_names[i]), free_std_string);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(swatch),
				theme_swatch_draw, nullptr, nullptr);
		gtk_box_append(GTK_BOX(rowbox), swatch);

		gtk_list_box_append(ui->theme_list, rowbox);
		if (ui->theme_names[i] == current)
			sel = static_cast<int>(i);
	}
	if (sel >= 0)
		gtk_list_box_select_row(ui->theme_list,
				gtk_list_box_get_row_at_index(ui->theme_list, sel));

	gtk_spin_button_set_value(ui->pad_y, s.titlebar_pad_y.value_or(2));
	gtk_spin_button_set_value(ui->button_size, s.titlebar_button_size.value_or(0));
	gtk_spin_button_set_value(ui->resize_grab, s.titlebar_resize_grab.value_or(8));

	gtk_drop_down_set_selected(ui->placement,
			index_of(kPlacement, s.placement_policy.value_or("Smart"), 0));

	gtk_spin_button_set_value(ui->margin_top, s.margin_top.value_or(0));
	gtk_spin_button_set_value(ui->margin_bottom, s.margin_bottom.value_or(0));
	gtk_spin_button_set_value(ui->margin_left, s.margin_left.value_or(0));
	gtk_spin_button_set_value(ui->margin_right, s.margin_right.value_or(0));

	gtk_editable_set_text(GTK_EDITABLE(ui->menu_source),
			s.menu_source.value_or("builtin").c_str());
	gtk_drop_down_set_selected(ui->submenu_open,
			index_of(kSubmenu, s.menu_submenu_open.value_or("hover"), 0));
	gtk_spin_button_set_value(ui->hover_delay, s.menu_hover_delay.value_or(100));
	gtk_switch_set_active(ui->menu_wrap, s.menu_wrap.value_or(true));
	gtk_switch_set_active(ui->menu_icons, s.menu_icons.value_or(true));

	gtk_drop_down_set_selected(ui->switcher_order,
			index_of(kOrder, s.switcher_order.value_or("mru"), 0));
	gtk_switch_set_active(ui->switcher_osd, s.switcher_osd.value_or(true));
	gtk_switch_set_active(ui->switcher_wrap, s.switcher_wrap.value_or(true));

	/* Always show a concrete font (the configured one, else waybox's effective
	 * default Sans 10) so the picker opens with the current font selected and
	 * the user can just tweak the size. */
	font_set(&ui->font_active, s.font_active_window.value_or("Sans 10"));
	font_set(&ui->font_inactive, s.font_inactive_window.value_or("Sans 10"));
	font_set(&ui->font_menu_header, s.font_menu_header.value_or("Sans 10"));
	font_set(&ui->font_menu_item, s.font_menu_item.value_or("Sans 10"));
	font_set(&ui->font_osd, s.font_osd.value_or("Sans 10"));

	preview_refresh(ui);
}

/* ---- Collect widget state back into a WayboxSettings ----------------- */

wb::WayboxSettings collect(Ui *ui) {
	wb::WayboxSettings s;
	if (GtkListBoxRow *r = gtk_list_box_get_selected_row(ui->theme_list)) {
		int idx = gtk_list_box_row_get_index(r);
		if (idx >= 0 && idx < static_cast<int>(ui->theme_names.size()))
			s.theme_name = ui->theme_names[idx];
	}
	s.titlebar_pad_y = gtk_spin_button_get_value_as_int(ui->pad_y);
	s.titlebar_button_size = gtk_spin_button_get_value_as_int(ui->button_size);
	s.titlebar_resize_grab = gtk_spin_button_get_value_as_int(ui->resize_grab);

	s.placement_policy = kPlacement[gtk_drop_down_get_selected(ui->placement)];

	s.margin_top = gtk_spin_button_get_value_as_int(ui->margin_top);
	s.margin_bottom = gtk_spin_button_get_value_as_int(ui->margin_bottom);
	s.margin_left = gtk_spin_button_get_value_as_int(ui->margin_left);
	s.margin_right = gtk_spin_button_get_value_as_int(ui->margin_right);

	s.menu_source = gtk_editable_get_text(GTK_EDITABLE(ui->menu_source));
	s.menu_submenu_open = kSubmenu[gtk_drop_down_get_selected(ui->submenu_open)];
	s.menu_hover_delay = gtk_spin_button_get_value_as_int(ui->hover_delay);
	s.menu_wrap = gtk_switch_get_active(ui->menu_wrap);
	s.menu_icons = gtk_switch_get_active(ui->menu_icons);

	s.switcher_order = kOrder[gtk_drop_down_get_selected(ui->switcher_order)];
	s.switcher_osd = gtk_switch_get_active(ui->switcher_osd);
	s.switcher_wrap = gtk_switch_get_active(ui->switcher_wrap);

	s.font_active_window = font_value(&ui->font_active);
	s.font_inactive_window = font_value(&ui->font_inactive);
	s.font_menu_header = font_value(&ui->font_menu_header);
	s.font_menu_item = font_value(&ui->font_menu_item);
	s.font_osd = font_value(&ui->font_osd);

	/* The font buttons are seeded with the effective default so the picker
	 * pre-selects a font; don't persist that default unless the user actually
	 * changed it from what was loaded. */
	auto keep = [](std::optional<std::string> &v,
			const std::optional<std::string> &orig) {
		if (!orig.has_value() && v == std::optional<std::string>("Sans 10"))
			v = std::nullopt;
	};
	keep(s.font_active_window, ui->original.font_active_window);
	keep(s.font_inactive_window, ui->original.font_inactive_window);
	keep(s.font_menu_header, ui->original.font_menu_header);
	keep(s.font_menu_item, ui->original.font_menu_item);
	keep(s.font_osd, ui->original.font_osd);
	return s;
}

void set_status(Ui *ui, const std::string &text) {
	if (ui->status != nullptr)
		gtk_label_set_text(ui->status, text.c_str());
}

void on_save(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	std::string path = user_rc_path();
	if (path.empty()) {
		set_status(ui, _("Could not determine the rc.xml path."));
		return;
	}
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);

	wb::RcDocument doc = load_document();
	doc.apply(collect(ui));
	if (!doc.save_file(path)) {
		set_status(ui, std::string(_("Failed to write ")) + path);
		return;
	}
	int n = signal_waybox_reload();
	if (n > 0)
		set_status(ui, _("Saved and reloaded waybox."));
	else
		set_status(ui, std::string(_("Saved to ")) + path + " " +
				_("(no running waybox to reload)."));
}

void on_close(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	gtk_window_close(ui->window);
}

/* ---- Reset to defaults ----------------------------------------------- */

/* The default values for one page, set straight onto its widgets, keyed by the
 * page's stack name so reset is robust to tab order/placeholder pages. */
void reset_page_widgets(Ui *ui, const std::string &name) {
	if (name == "theme") {
		gtk_list_box_unselect_all(ui->theme_list);
	} else if (name == "appearance") {
		gtk_spin_button_set_value(ui->pad_y, 2);
		gtk_spin_button_set_value(ui->button_size, 0);
		gtk_spin_button_set_value(ui->resize_grab, 8);
	} else if (name == "fonts") {
		font_set(&ui->font_active, "Sans 10");
		font_set(&ui->font_inactive, "Sans 10");
		font_set(&ui->font_menu_header, "Sans 10");
		font_set(&ui->font_menu_item, "Sans 10");
		font_set(&ui->font_osd, "Sans 10");
	} else if (name == "windows") {
		gtk_drop_down_set_selected(ui->placement, 0);  /* Smart */
	} else if (name == "margins") {
		gtk_spin_button_set_value(ui->margin_top, 0);
		gtk_spin_button_set_value(ui->margin_bottom, 0);
		gtk_spin_button_set_value(ui->margin_left, 0);
		gtk_spin_button_set_value(ui->margin_right, 0);
	} else if (name == "menu") {
		gtk_editable_set_text(GTK_EDITABLE(ui->menu_source), "builtin");
		gtk_drop_down_set_selected(ui->submenu_open, 0);  /* hover */
		gtk_spin_button_set_value(ui->hover_delay, 100);
		gtk_switch_set_active(ui->menu_wrap, TRUE);
		gtk_switch_set_active(ui->menu_icons, TRUE);
	} else if (name == "switcher") {
		gtk_drop_down_set_selected(ui->switcher_order, 0);  /* mru */
		gtk_switch_set_active(ui->switcher_osd, TRUE);
		gtk_switch_set_active(ui->switcher_wrap, TRUE);
	}
	preview_refresh(ui);
}

void on_reset_page(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	const char *name = gtk_stack_get_visible_child_name(ui->stack);
	reset_page_widgets(ui, name != nullptr ? name : "");
	set_status(ui, _("Reset this page to defaults (not yet saved)."));
}

void on_reset_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	for (const char *name : {"theme", "appearance", "fonts", "windows",
			"margins", "menu", "switcher"})
		reset_page_widgets(ui, name);
	set_status(ui, _("Reset all settings to defaults (not yet saved)."));
}

GtkWidget *build_theme_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Theme"));

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroller, TRUE);

	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
	ui->theme_list = GTK_LIST_BOX(list);
	g_signal_connect_swapped(list, "selected-rows-changed",
			G_CALLBACK(preview_refresh), ui);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
	gtk_box_append(GTK_BOX(content), scroller);

	/* Live preview pane below the theme list. */
	GtkWidget *pcontent = section(page, _("Preview"));
	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, -1, 230);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), preview_draw, ui,
			nullptr);
	ui->preview = area;
	gtk_box_append(GTK_BOX(pcontent), area);
	return page;
}

GtkWidget *build_fonts_page(Ui *ui) {
	GtkWidget *page = make_page();

	/* Per-place pickers. Close a row's padlock to link the others to it. */
	GtkWidget *content = section(page, _("Fonts"));
	GtkWidget *hint = gtk_label_new(
			_("Tip: close a row's padlock to link the other fonts to it."));
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
	gtk_box_append(GTK_BOX(content), hint);
	GtkWidget *grid = section_grid(content);
	add_font(grid, 0, _("Active window title:"), &ui->font_active, ui, 0);
	add_font(grid, 1, _("Inactive window title:"), &ui->font_inactive, ui, 1);
	add_font(grid, 2, _("Menu header:"), &ui->font_menu_header, ui, 2);
	add_font(grid, 3, _("Menu item:"), &ui->font_menu_item, ui, 3);
	add_font(grid, 4, _("On-screen display:"), &ui->font_osd, ui, 4);
	return page;
}

GtkWidget *build_appearance_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Window Titles"));
	GtkWidget *grid = section_grid(content);
	ui->pad_y = add_spin(grid, 0, _("Titlebar padding (px):"), 0, 16);
	ui->button_size = add_spin(grid, 1, _("Button size (px, 0 = auto):"), 0, 64);
	ui->resize_grab = add_spin(grid, 2, _("Resize grab margin (px):"), 0, 32);
	return page;
}

GtkWidget *build_windows_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Placing Windows"));
	GtkWidget *grid = section_grid(content);
	ui->placement = add_dropdown(grid, 0, _("Prefer to place windows:"),
			kPlacement);
	return page;
}

GtkWidget *build_margins_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Reserved Screen Margins"));
	GtkWidget *grid = section_grid(content);
	ui->margin_top = add_spin(grid, 0, _("Top:"), 0, 1024);
	ui->margin_bottom = add_spin(grid, 1, _("Bottom:"), 0, 1024);
	ui->margin_left = add_spin(grid, 2, _("Left:"), 0, 1024);
	ui->margin_right = add_spin(grid, 3, _("Right:"), 0, 1024);
	return page;
}

GtkWidget *build_menu_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Root Menu"));
	GtkWidget *grid = section_grid(content);
	grid_label(grid, 0, _("Source (builtin or command):"));
	ui->menu_source = GTK_ENTRY(grid_control(grid, 0, gtk_entry_new()));
	ui->submenu_open = add_dropdown(grid, 1, _("Submenu opens on:"), kSubmenu);
	ui->hover_delay = add_spin(grid, 2, _("Submenu hover delay (ms):"), 0, 2000);
	ui->menu_wrap = add_switch(grid, 3, _("Wrap selection:"));
	ui->menu_icons = add_switch(grid, 4, _("Show icons:"));
	return page;
}

GtkWidget *build_switcher_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Task Switcher"));
	GtkWidget *grid = section_grid(content);
	ui->switcher_order = add_dropdown(grid, 0, _("Order:"), kOrder);
	ui->switcher_osd = add_switch(grid, 1, _("Show on-screen display:"));
	ui->switcher_wrap = add_switch(grid, 2, _("Wrap selection:"));
	return page;
}

/* A placeholder page for obconf tabs we don't yet back with settings, so the
 * full tab structure is present and honest about what's wired up. */
GtkWidget *build_placeholder_page(const char *title, const char *note) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, title);
	GtkWidget *lbl = gtk_label_new(note);
	gtk_widget_add_css_class(lbl, "dim-label");
	gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_widget_set_halign(lbl, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(content), lbl);
	return page;
}

void on_about(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	GtkWidget *dlg = gtk_about_dialog_new();
	GtkAboutDialog *about = GTK_ABOUT_DIALOG(dlg);
	gtk_about_dialog_set_program_name(about, _("Waybox Configuration Manager"));
#ifdef PACKAGE_VERSION
	gtk_about_dialog_set_version(about, PACKAGE_VERSION);
#endif
	gtk_about_dialog_set_comments(about,
			_("Settings for the Waybox Wayland compositor."));
	gtk_about_dialog_set_website(about, "https://github.com/TheCleaners/waybox");
	gtk_about_dialog_set_website_label(about, _("Project page"));
	gtk_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
	gtk_about_dialog_set_logo_icon_name(about, "preferences-system");
	gtk_window_set_transient_for(GTK_WINDOW(dlg), ui->window);
	gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
	gtk_window_present(GTK_WINDOW(dlg));
}

void add_page(Ui *ui, GtkWidget *page, const char *name, const char *title) {
	gtk_stack_add_titled(ui->stack, page, name, title);
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Configuration Manager"));
	gtk_window_set_default_size(ui->window, 720, 520);

	/* An explicit header bar: GTK then owns a proper, draggable client-side
	 * titlebar (a GtkWindowHandle) that drives compositor move/maximize
	 * correctly. Without one, a decorated GtkWindow falls back to a degenerate
	 * titlebar that misbehaves under a wlroots compositor (the window can jump
	 * or resize on drag). waybox leaves GTK windows client-side decorated
	 * because GTK does not implement the server-side xdg-decoration protocol. */
	GtkWidget *header = gtk_header_bar_new();
	gtk_window_set_titlebar(ui->window, header);

	GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic");
	gtk_widget_set_tooltip_text(about_btn, _("About"));
	g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about), ui);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_window_set_child(ui->window, vbox);

	/* Vertical tabs (obconf-style): a sidebar list on the left driving a stack
	 * of pages on the right. */
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(hbox, TRUE);
	gtk_box_append(GTK_BOX(vbox), hbox);

	GtkWidget *stack = gtk_stack_new();
	gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);
	ui->stack = GTK_STACK(stack);

	GtkWidget *sidebar = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
	gtk_box_append(GTK_BOX(hbox), sidebar);

	GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
	gtk_box_append(GTK_BOX(hbox), sep);

	gtk_widget_set_hexpand(stack, TRUE);
	gtk_box_append(GTK_BOX(hbox), stack);

	/* obconf's tab order, plus waybox-specific pages. Placeholders mark tabs
	 * whose backing settings aren't wired up yet (Display lands later). */
	add_page(ui, build_theme_page(ui), "theme", _("Theme"));
	add_page(ui, build_appearance_page(ui), "appearance", _("Appearance"));
	add_page(ui, build_windows_page(ui), "windows", _("Windows"));
	add_page(ui, build_placeholder_page(_("Move & Resize"),
			_("Window move/resize behaviour is not configurable here yet.")),
			"moveresize", _("Move & Resize"));
	add_page(ui, build_placeholder_page(_("Mouse"),
			_("Mouse bindings and focus options are not configurable here "
			  "yet.")), "mouse", _("Mouse"));
	add_page(ui, build_placeholder_page(_("Desktops"),
			_("Virtual desktops are not implemented yet.")),
			"desktops", _("Desktops"));
	add_page(ui, build_margins_page(ui), "margins", _("Margins"));
	add_page(ui, build_placeholder_page(_("Dock"),
			_("Panel/dock placement is managed by your layer-shell panel.")),
			"dock", _("Dock"));
	add_page(ui, build_menu_page(ui), "menu", _("Menu"));
	add_page(ui, build_switcher_page(ui), "switcher", _("Switcher"));
	add_page(ui, build_fonts_page(ui), "fonts", _("Fonts"));

	/* Action row + status line. */
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(actions, 12);
	gtk_widget_set_margin_end(actions, 12);
	gtk_widget_set_margin_bottom(actions, 12);

	GtkWidget *status = gtk_label_new("");
	gtk_widget_set_hexpand(status, TRUE);
	gtk_widget_set_halign(status, GTK_ALIGN_START);
	ui->status = GTK_LABEL(status);
	gtk_box_append(GTK_BOX(actions), status);

	/* Reset: a split button — the main part resets the current page, the arrow
	 * opens a menu with "Reset This Page" / "Reset All to Defaults". GTK core
	 * has no split-button widget, so compose a linked button + menu button. */
	GtkWidget *reset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(reset_box, "linked");

	GtkWidget *reset_primary = gtk_button_new_with_label(_("Reset Page"));
	g_signal_connect(reset_primary, "clicked", G_CALLBACK(on_reset_page), ui);
	gtk_box_append(GTK_BOX(reset_box), reset_primary);

	GtkWidget *reset_menu = gtk_menu_button_new();
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_margin_top(pbox, 4);
	gtk_widget_set_margin_bottom(pbox, 4);
	gtk_widget_set_margin_start(pbox, 4);
	gtk_widget_set_margin_end(pbox, 4);

	GtkWidget *rp = gtk_button_new_with_label(_("Reset This Page"));
	gtk_button_set_has_frame(GTK_BUTTON(rp), FALSE);
	gtk_widget_set_halign(rp, GTK_ALIGN_FILL);
	g_signal_connect(rp, "clicked", G_CALLBACK(on_reset_page), ui);
	g_signal_connect_swapped(rp, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	gtk_box_append(GTK_BOX(pbox), rp);

	GtkWidget *ra = gtk_button_new_with_label(_("Reset All to Defaults"));
	gtk_button_set_has_frame(GTK_BUTTON(ra), FALSE);
	gtk_widget_set_halign(ra, GTK_ALIGN_FILL);
	g_signal_connect(ra, "clicked", G_CALLBACK(on_reset_all), ui);
	g_signal_connect_swapped(ra, "clicked", G_CALLBACK(gtk_popover_popdown), pop);
	gtk_box_append(GTK_BOX(pbox), ra);

	gtk_popover_set_child(GTK_POPOVER(pop), pbox);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(reset_menu), pop);
	gtk_box_append(GTK_BOX(reset_box), reset_menu);
	gtk_box_append(GTK_BOX(actions), reset_box);

	GtkWidget *close = gtk_button_new_with_label(_("Close"));
	g_signal_connect(close, "clicked", G_CALLBACK(on_close), ui);
	gtk_box_append(GTK_BOX(actions), close);

	GtkWidget *save = gtk_button_new_with_label(_("Save & Apply"));
	gtk_widget_add_css_class(save, "suggested-action");
	g_signal_connect(save, "clicked", G_CALLBACK(on_save), ui);
	gtk_box_append(GTK_BOX(actions), save);

	gtk_box_append(GTK_BOX(vbox), actions);

	populate(ui, load_document().read());
	gtk_window_present(ui->window);
}

void on_activate(GtkApplication *app, gpointer data) {
	build_window(app, static_cast<Ui *>(data));
}

}  // namespace

int main(int argc, char **argv) {
	setlocale(LC_ALL, "");
#ifdef LOCALEDIR
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
#endif
	textdomain(GETTEXT_PACKAGE);

	Ui ui;
	GtkApplication *app = gtk_application_new("org.waybox.wbconf",
			G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
