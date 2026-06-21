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

struct FontPicker {
	GtkDropDown *family = nullptr;
	GtkSpinButton *size = nullptr;
	GtkToggleButton *bold = nullptr;
	std::vector<std::string> families;  /* parallel to the dropdown model */
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

	/* "Apply to all" controls at the top of the Fonts page. */
	GtkDropDown *bulk_family = nullptr;
	GtkSpinButton *bulk_size = nullptr;

	/* Live theme preview (redrawn when the theme/fonts change). */
	GtkWidget *preview = nullptr;
	GtkNotebook *notebook = nullptr;  /* for "reset this page" */

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

/* A custom, instant font picker: a family dropdown (each item rendered in its
 * own font), a size spin button and a bold toggle, laid out in one row. */
void add_font(GtkWidget *grid, int row, const char *label, FontPicker *out,
		GCallback on_change, gpointer user) {
	grid_label(grid, row, label);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_hexpand(box, TRUE);

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

	if (on_change != nullptr) {
		g_signal_connect_swapped(fam, "notify::selected", on_change, user);
		g_signal_connect_swapped(size, "value-changed", on_change, user);
		g_signal_connect_swapped(bold, "toggled", on_change, user);
	}
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

/* All five font pickers, for the "apply to all" bulk controls. */
std::vector<FontPicker *> all_pickers(Ui *ui) {
	return {&ui->font_active, &ui->font_inactive, &ui->font_menu_header,
			&ui->font_menu_item, &ui->font_osd};
}

/* Apply the bulk family selection to every row. */
void on_apply_family_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	guint idx = gtk_drop_down_get_selected(ui->bulk_family);
	for (FontPicker *p : all_pickers(ui)) {
		if (idx != GTK_INVALID_LIST_POSITION && idx < p->families.size())
			gtk_drop_down_set_selected(p->family, idx);
	}
	set_status(ui, _("Applied the family to all fonts (not yet saved)."));
	preview_refresh(ui);
}

/* Apply the bulk size to every row. */
void on_apply_size_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int sz = gtk_spin_button_get_value_as_int(ui->bulk_size);
	for (FontPicker *p : all_pickers(ui))
		gtk_spin_button_set_value(p->size, sz);
	set_status(ui, _("Applied the size to all fonts (not yet saved)."));
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
		GtkWidget *label = gtk_label_new(ui->theme_names[i].c_str());
		gtk_widget_set_halign(label, GTK_ALIGN_START);
		gtk_widget_set_margin_top(label, 4);
		gtk_widget_set_margin_bottom(label, 4);
		gtk_widget_set_margin_start(label, 6);
		gtk_widget_set_margin_end(label, 6);
		gtk_list_box_append(ui->theme_list, label);
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

/* The default values for one notebook page, set straight onto its widgets.
 * Page order matches build_window: 0 Theme, 1 Appearance, 2 Fonts, 3 Windows,
 * 4 Margins, 5 Menu, 6 Switcher. */
void reset_page_widgets(Ui *ui, int page) {
	switch (page) {
	case 0:  /* Theme: clear the selection (built-in default theme). */
		gtk_list_box_unselect_all(ui->theme_list);
		break;
	case 1:  /* Appearance */
		gtk_spin_button_set_value(ui->pad_y, 2);
		gtk_spin_button_set_value(ui->button_size, 0);
		gtk_spin_button_set_value(ui->resize_grab, 8);
		break;
	case 2:  /* Fonts */
		font_set(&ui->font_active, "Sans 10");
		font_set(&ui->font_inactive, "Sans 10");
		font_set(&ui->font_menu_header, "Sans 10");
		font_set(&ui->font_menu_item, "Sans 10");
		font_set(&ui->font_osd, "Sans 10");
		break;
	case 3:  /* Windows */
		gtk_drop_down_set_selected(ui->placement, 0);  /* Smart */
		break;
	case 4:  /* Margins */
		gtk_spin_button_set_value(ui->margin_top, 0);
		gtk_spin_button_set_value(ui->margin_bottom, 0);
		gtk_spin_button_set_value(ui->margin_left, 0);
		gtk_spin_button_set_value(ui->margin_right, 0);
		break;
	case 5:  /* Menu */
		gtk_editable_set_text(GTK_EDITABLE(ui->menu_source), "builtin");
		gtk_drop_down_set_selected(ui->submenu_open, 0);  /* hover */
		gtk_spin_button_set_value(ui->hover_delay, 100);
		gtk_switch_set_active(ui->menu_wrap, TRUE);
		gtk_switch_set_active(ui->menu_icons, TRUE);
		break;
	case 6:  /* Switcher */
		gtk_drop_down_set_selected(ui->switcher_order, 0);  /* mru */
		gtk_switch_set_active(ui->switcher_osd, TRUE);
		gtk_switch_set_active(ui->switcher_wrap, TRUE);
		break;
	}
	preview_refresh(ui);
}

void on_reset_page(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	int page = gtk_notebook_get_current_page(ui->notebook);
	reset_page_widgets(ui, page);
	set_status(ui, _("Reset this page to defaults (not yet saved)."));
}

void on_reset_all(GtkButton *, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	gint pages = gtk_notebook_get_n_pages(ui->notebook);
	for (gint i = 0; i < pages; i++)
		reset_page_widgets(ui, i);
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

	/* "Apply to all" toolbar: pick a family or a size and push it to every row
	 * at once (handy for scaling the whole UI or switching one typeface). */
	GtkWidget *bulk = section(page, _("Apply to all fonts"));
	GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	std::vector<std::string> fams = font_families();
	std::vector<const char *> items;
	for (const std::string &f : fams)
		items.push_back(f.c_str());
	items.push_back(nullptr);
	GtkWidget *bfam = gtk_drop_down_new_from_strings(items.data());
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(bfam), TRUE);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(family_item_setup), nullptr);
	g_signal_connect(factory, "bind", G_CALLBACK(family_item_bind), nullptr);
	gtk_drop_down_set_factory(GTK_DROP_DOWN(bfam), factory);
	g_object_unref(factory);
	gtk_widget_set_hexpand(bfam, TRUE);
	ui->bulk_family = GTK_DROP_DOWN(bfam);
	gtk_box_append(GTK_BOX(brow), bfam);
	GtkWidget *set_fam = gtk_button_new_with_label(_("Set family"));
	g_signal_connect(set_fam, "clicked", G_CALLBACK(on_apply_family_all), ui);
	gtk_box_append(GTK_BOX(brow), set_fam);

	GtkWidget *bsize = gtk_spin_button_new_with_range(4, 96, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(bsize), 10);
	ui->bulk_size = GTK_SPIN_BUTTON(bsize);
	gtk_box_append(GTK_BOX(brow), bsize);
	GtkWidget *set_size = gtk_button_new_with_label(_("Set size"));
	g_signal_connect(set_size, "clicked", G_CALLBACK(on_apply_size_all), ui);
	gtk_box_append(GTK_BOX(brow), set_size);

	gtk_box_append(GTK_BOX(bulk), brow);

	/* Per-place pickers. */
	GtkWidget *content = section(page, _("Fonts"));
	GtkWidget *grid = section_grid(content);
	GCallback refresh = G_CALLBACK(preview_refresh);
	add_font(grid, 0, _("Active window title:"), &ui->font_active, refresh, ui);
	add_font(grid, 1, _("Inactive window title:"), &ui->font_inactive, refresh, ui);
	add_font(grid, 2, _("Menu header:"), &ui->font_menu_header, refresh, ui);
	add_font(grid, 3, _("Menu item:"), &ui->font_menu_item, refresh, ui);
	add_font(grid, 4, _("On-screen display:"), &ui->font_osd, refresh, ui);
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

void add_tab(GtkWidget *notebook, GtkWidget *page, const char *label) {
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new(label));
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Configuration Manager"));
	gtk_window_set_default_size(ui->window, 500, 460);

	/* An explicit header bar: GTK then owns a proper, draggable client-side
	 * titlebar (a GtkWindowHandle) that drives compositor move/maximize
	 * correctly. Without one, a decorated GtkWindow falls back to a degenerate
	 * titlebar that misbehaves under a wlroots compositor (the window can jump
	 * or resize on drag). waybox leaves GTK windows client-side decorated
	 * because GTK does not implement the server-side xdg-decoration protocol. */
	GtkWidget *header = gtk_header_bar_new();
	gtk_window_set_titlebar(ui->window, header);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_window_set_child(ui->window, vbox);

	GtkWidget *notebook = gtk_notebook_new();
	gtk_widget_set_vexpand(notebook, TRUE);
	gtk_box_append(GTK_BOX(vbox), notebook);
	ui->notebook = GTK_NOTEBOOK(notebook);

	add_tab(notebook, build_theme_page(ui), _("Theme"));
	add_tab(notebook, build_appearance_page(ui), _("Appearance"));
	add_tab(notebook, build_fonts_page(ui), _("Fonts"));
	add_tab(notebook, build_windows_page(ui), _("Windows"));
	add_tab(notebook, build_margins_page(ui), _("Margins"));
	add_tab(notebook, build_menu_page(ui), _("Menu"));
	add_tab(notebook, build_switcher_page(ui), _("Switcher"));

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
