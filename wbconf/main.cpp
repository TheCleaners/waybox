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
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <cstring>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
 * default (so the user inherits its settings), else a blank skeleton. A user
 * file with an error is NOT replaced by the default: nullopt, with the error,
 * so nothing is saved over it. */
std::optional<wb::RcDocument> load_document(std::string *problem) {
	std::error_code ec;
	std::string up = user_rc_path();
	if (!up.empty() && fs::exists(up, ec)) {
		wb::LoadError err;
		if (auto d = wb::RcDocument::load_file(up, &err))
			return d;
		if (problem != nullptr) {
			char *msg = g_strdup_printf(_("%s, line %d: %s. Fix it (wbconf won't save "
					"over it), then reopen wbconf."), up.c_str(), err.line,
					err.message.c_str());
			*problem = msg;
			g_free(msg);
		}
		return std::nullopt;
	}
	std::string sp = system_rc_path();
	if (!sp.empty() && fs::exists(sp, ec))
		if (auto d = wb::RcDocument::load_file(sp))
			return d;
	return wb::RcDocument::from_string(kSkeletonRc);
}

/* The process at the other end of this session's Wayland socket: the
 * compositor wbconf is running in. 0 if there is none. */
pid_t session_compositor() {
	const char *display = getenv("WAYLAND_DISPLAY");
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (display == nullptr || display[0] == '\0')
		return 0;
	std::string path = display[0] == '/' ? display
			: (runtime ? std::string(runtime) + "/" + display : std::string());
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (path.empty() || path.size() >= sizeof(addr.sun_path))
		return 0;
	std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	pid_t pid = 0;
	struct ucred cred = {};
	socklen_t len = sizeof(cred);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 &&
			getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
		pid = cred.pid;
	close(fd);
	return pid;
}

bool is_waybox(long pid) {
	std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
	std::string name;
	return std::getline(comm, name) && name == "waybox";
}

/* Ask Waybox to reconfigure (SIGUSR2, the signal Openbox uses): the one
 * wbconf runs in; outside a Waybox session, every Waybox this user runs.
 * Returns the number signalled. */
int signal_waybox_reload() {
	if (pid_t pid = session_compositor(); pid > 0 && is_waybox(pid))
		return kill(pid, SIGUSR2) == 0 ? 1 : 0;
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
		if (!is_waybox(pid))
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

/* ---- Monitors, as wlr-randr --json reports them ----------------------- */

struct MonitorInfo {
	std::string name;         /* connector */
	std::string description;  /* make, model */
	std::vector<std::string> modes;  /* "WxH@HZ" */
};

/* "60" for 60.000, "59.997" otherwise. */
std::string format_hz(double hz) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", hz);
	std::string s = buf;
	while (!s.empty() && s.back() == '0')
		s.pop_back();
	if (!s.empty() && s.back() == '.')
		s.pop_back();
	return s;
}

/* The connected monitors, or nullopt (with why) when wlr-randr can't say. */
std::optional<std::vector<MonitorInfo>> query_monitors(std::string *why) {
	gchar *out = nullptr;
	gint status = 0;
	GError *err = nullptr;
	if (!g_spawn_command_line_sync("wlr-randr --json", &out, nullptr, &status, &err) ||
			!g_spawn_check_wait_status(status, nullptr)) {
		*why = _("Monitors are listed with wlr-randr; install it to set them here.");
		if (err != nullptr)
			g_error_free(err);
		g_free(out);
		return std::nullopt;
	}
	std::vector<MonitorInfo> list;
	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_data(parser, out, -1, nullptr)) {
		JsonNode *root = json_parser_get_root(parser);
		JsonArray *arr = JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : nullptr;
		for (guint i = 0; arr != nullptr && i < json_array_get_length(arr); i++) {
			JsonObject *o = json_array_get_object_element(arr, i);
			if (o == nullptr || !json_object_has_member(o, "name"))
				continue;
			MonitorInfo m;
			m.name = json_object_get_string_member(o, "name");
			if (json_object_has_member(o, "description"))
				m.description = json_object_get_string_member(o, "description");
			if (json_object_has_member(o, "modes")) {
				JsonArray *modes = json_object_get_array_member(o, "modes");
				for (guint j = 0; j < json_array_get_length(modes); j++) {
					JsonObject *md = json_array_get_object_element(modes, j);
					std::string v = std::to_string(json_object_get_int_member(md, "width")) +
							"x" + std::to_string(json_object_get_int_member(md, "height"));
					double hz = json_object_get_double_member(md, "refresh");
					if (hz > 0)
						v += "@" + format_hz(hz);
					if (std::find(m.modes.begin(), m.modes.end(), v) == m.modes.end())
						m.modes.push_back(v);
				}
			}
			list.push_back(m);
		}
	}
	g_object_unref(parser);
	g_free(out);
	if (list.empty())
		*why = _("No monitors found.");
	return list;
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
	GtkEntry *title_layout = nullptr;

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
	GtkSwitch *switcher_icons = nullptr;

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

	/* Desktops */
	GtkSpinButton *desktops_number = nullptr;
	GtkTextView *desktop_names = nullptr;
	GtkSpinButton *popup_time = nullptr;

	/* Edge tiling (on the Windows page) */
	GtkSwitch *edge_tiling = nullptr;
	GtkSpinButton *edge_range = nullptr;

	/* Monitors: one block per connected monitor. Each dropdown's values are
	 * kept beside it; "" (the first) is Automatic. */
	struct Monitor {
		std::string name;
		GtkSwitch *enabled = nullptr;
		GtkDropDown *mode = nullptr;
		std::vector<std::string> modes;
		GtkDropDown *scale = nullptr;
		std::vector<std::string> scales;
		GtkEntry *position = nullptr;
		GtkDropDown *transform = nullptr;
		std::map<std::string, std::string> kept;  /* attributes not shown here */
	};
	std::vector<Monitor> monitors;
	/* Rules for monitors that aren't connected now: passed through as is. */
	std::vector<wb::OutputSetting> other_outputs;

	/* Keyboard: <xkb> fields */
	std::map<std::string, GtkEntry *> xkb;

	/* Mouse and touchpad: one dropdown per <libinput> setting, with its
	 * values ("" = the device's default, not written). */
	struct Choice {
		GtkDropDown *dd = nullptr;
		std::vector<std::string> values;
	};
	std::map<std::string, Choice> libinput;

	/* Advanced */
	GtkSwitch *xwayland = nullptr;
	Choice virtual_input;

	/* HDR: per-monitor checks; names in <hdr outputs> for monitors that
	 * aren't connected; and the attribute as loaded (kept as written when
	 * the set of names doesn't change). */
	std::map<std::string, GtkCheckButton *> hdr;
	std::vector<std::string> hdr_others;
	std::optional<std::string> hdr_loaded;

	/* Live theme preview (redrawn when the theme/fonts change). */
	GtkWidget *preview = nullptr;
	GtkNotebook *notebook = nullptr;  /* for "reset this page" */

	/* What the widgets showed after loading (or the last save): Save writes
	 * only what differs from it, so untouched settings, including the
	 * defaults the widgets display for absent ones, aren't written. */
	wb::WayboxSettings shown;
	GtkWidget *save = nullptr;
	bool loaded = false;  /* false: rc.xml has an error; nothing is saved */
};

const char *const kPlacement[] = {"Smart", "Center", "UnderMouse", nullptr};
const char *const kSubmenu[] = {"hover", "click", nullptr};
const char *const kOrder[] = {"mru", "stacking", "spatial", nullptr};
std::vector<std::string> kTransforms = {"", "normal", "90", "180", "270", "flipped",
		"flipped-90", "flipped-180", "flipped-270"};

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

/* A custom, instant font picker: a family dropdown, a size spin button and a
 * bold toggle, laid out in one row. Stored in `out`. */
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
	/* Make the long list searchable by typing. The search needs an
	 * expression naming the text to match (the GtkStringObject's "string");
	 * without one it matches nothing and the list never filters. Match
	 * anywhere in the name ("mono" finds "DejaVu Sans Mono"), not just the
	 * start. */
	gtk_drop_down_set_expression(GTK_DROP_DOWN(fam),
			gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(fam), TRUE);
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(fam),
			GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
#endif
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

	/* A family that isn't installed stays as it is (an extra entry), rather
	 * than becoming whichever font happens to be first. */
	auto it = std::find(p->families.begin(), p->families.end(), family);
	if (it == p->families.end()) {
		gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(p->family)),
				family.c_str());
		p->families.push_back(family);
		it = p->families.end() - 1;
	}
	gtk_drop_down_set_selected(p->family,
			static_cast<guint>(it - p->families.begin()));
	gtk_spin_button_set_value(p->size, size_pt);
	gtk_toggle_button_set_active(p->bold, bold);
}

/* Collect the current settings from all widgets (forward declaration so the
 * preview can render the live, unsaved state). */
struct Ui;
wb::WayboxSettings collect(Ui *ui);

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

/* A dropdown over `values`, "" shown as Automatic. */
GtkDropDown *values_dropdown(const std::vector<std::string> &values) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	for (const std::string &v : values)
		gtk_string_list_append(model, v.empty() ? _("Automatic") : v.c_str());
	return GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), nullptr));
}

/* A dropdown over values with their own labels. */
GtkDropDown *choice_dropdown(const std::vector<std::pair<std::string, std::string>> &items,
		std::vector<std::string> &values) {
	GtkStringList *model = gtk_string_list_new(nullptr);
	values.clear();
	for (const auto &[value, label] : items) {
		gtk_string_list_append(model, label.c_str());
		values.push_back(value);
	}
	return GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), nullptr));
}

std::vector<std::string> split_names(const std::string &list) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : list + " ") {
		if (c == ' ' || c == ',' || c == '\t') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	return out;
}

/* Select `value` in a values dropdown, adding it if it isn't listed (so a
 * value written by hand stays exactly as written). */
void select_value(GtkDropDown *dd, std::vector<std::string> &values,
		const std::string &value) {
	auto it = std::find(values.begin(), values.end(), value);
	if (it == values.end()) {
		gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(dd)),
				value.c_str());
		values.push_back(value);
		it = values.end() - 1;
	}
	gtk_drop_down_set_selected(dd, static_cast<guint>(it - values.begin()));
}

std::string selected_value(GtkDropDown *dd, const std::vector<std::string> &values) {
	guint i = gtk_drop_down_get_selected(dd);
	return i < values.size() ? values[i] : std::string();
}

void populate_monitors(Ui *ui, const std::vector<wb::OutputSetting> &outputs) {
	ui->other_outputs.clear();
	for (const wb::OutputSetting &o : outputs) {
		bool connected = false;
		for (const Ui::Monitor &m : ui->monitors)
			connected = connected || m.name == o.name;
		if (!connected)
			ui->other_outputs.push_back(o);
	}
	for (Ui::Monitor &m : ui->monitors) {
		std::map<std::string, std::string> attrs;
		for (const wb::OutputSetting &o : outputs)
			if (o.name == m.name)
				attrs = o.attrs;
		auto take = [&attrs](const char *key) {
			auto it = attrs.find(key);
			std::string v = it == attrs.end() ? "" : it->second;
			if (it != attrs.end())
				attrs.erase(it);
			return v;
		};
		std::string enabled = take("enabled");
		gtk_switch_set_active(m.enabled, enabled != "no");
		if (!enabled.empty() && enabled != "no")
			attrs["enabled"] = enabled;  /* an explicit "yes" stays */
		select_value(m.mode, m.modes, take("mode"));
		select_value(m.scale, m.scales, take("scale"));
		gtk_editable_set_text(GTK_EDITABLE(m.position), take("position").c_str());
		select_value(m.transform, kTransforms, take("transform"));
		m.kept = attrs;
	}
}

void populate(Ui *ui, const wb::WayboxSettings &s) {
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
	if (sel >= 0) {
		GtkListBoxRow *selected = gtk_list_box_get_row_at_index(ui->theme_list, sel);
		gtk_list_box_select_row(ui->theme_list, selected);
		/* Scroll it into view once the list is laid out (focusing a row does). */
		g_idle_add_once([](gpointer row) { gtk_widget_grab_focus(GTK_WIDGET(row)); },
				selected);
	}

	gtk_spin_button_set_value(ui->pad_y, s.titlebar_pad_y.value_or(2));
	gtk_spin_button_set_value(ui->button_size, s.titlebar_button_size.value_or(0));
	gtk_spin_button_set_value(ui->resize_grab, s.titlebar_resize_grab.value_or(8));
	gtk_editable_set_text(GTK_EDITABLE(ui->title_layout),
			s.title_layout.value_or("NLIMC").c_str());

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
	gtk_switch_set_active(ui->switcher_icons, s.switcher_icons.value_or(true));

	/* Always show a concrete font (the configured one, else waybox's effective
	 * default Sans 10) so the picker opens with the current font selected and
	 * the user can just tweak the size. */
	font_set(&ui->font_active, s.font_active_window.value_or("Sans 10"));
	font_set(&ui->font_inactive, s.font_inactive_window.value_or("Sans 10"));
	font_set(&ui->font_menu_header, s.font_menu_header.value_or("Sans 10"));
	font_set(&ui->font_menu_item, s.font_menu_item.value_or("Sans 10"));
	font_set(&ui->font_osd, s.font_osd.value_or("Sans 10"));

	gtk_spin_button_set_value(ui->desktops_number, s.desktops_number.value_or(4));
	std::string names;
	for (const std::string &n : s.desktop_names.value_or(std::vector<std::string>{}))
		names += n + "\n";
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(ui->desktop_names),
			names.c_str(), -1);
	gtk_spin_button_set_value(ui->popup_time, s.desktop_popup_time.value_or(875));

	gtk_switch_set_active(ui->edge_tiling, s.edge_tiling.value_or(true));
	gtk_spin_button_set_value(ui->edge_range, s.edge_tiling_range.value_or(10));

	populate_monitors(ui, s.outputs);

	ui->hdr_loaded = s.hdr_outputs;
	ui->hdr_others.clear();
	std::vector<std::string> hdr = split_names(s.hdr_outputs.value_or(""));
	for (const std::string &n : hdr)
		if (ui->hdr.count(n) == 0)
			ui->hdr_others.push_back(n);
	for (auto &[name, check] : ui->hdr)
		gtk_check_button_set_active(check,
				std::find(hdr.begin(), hdr.end(), name) != hdr.end());

	for (auto &[key, entry] : ui->xkb) {
		auto it = s.xkb.find(key);
		gtk_editable_set_text(GTK_EDITABLE(entry), it == s.xkb.end() ? "" : it->second.c_str());
	}
	for (auto &[key, choice] : ui->libinput) {
		auto it = s.libinput.find(key);
		select_value(choice.dd, choice.values, it == s.libinput.end() ? "" : it->second);
	}
	gtk_switch_set_active(ui->xwayland, s.xwayland.value_or(true));
	select_value(ui->virtual_input.dd, ui->virtual_input.values,
			s.virtual_input.value_or("unlocked"));

	ui->shown = collect(ui);
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
	{
		/* Only Openbox's letters, upper case. */
		std::string layout;
		for (char c : std::string(gtk_editable_get_text(GTK_EDITABLE(ui->title_layout)))) {
			char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if (std::string("NLIMCSD").find(u) != std::string::npos)
				layout += u;
		}
		s.title_layout = layout;
	}

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
	s.switcher_icons = gtk_switch_get_active(ui->switcher_icons);

	s.font_active_window = font_value(&ui->font_active);
	s.font_inactive_window = font_value(&ui->font_inactive);
	s.font_menu_header = font_value(&ui->font_menu_header);
	s.font_menu_item = font_value(&ui->font_menu_item);
	s.font_osd = font_value(&ui->font_osd);

	s.desktops_number = gtk_spin_button_get_value_as_int(ui->desktops_number);
	GtkTextBuffer *buf = gtk_text_view_get_buffer(ui->desktop_names);
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buf, &start, &end);
	gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
	std::vector<std::string> names;
	std::istringstream lines(text);
	for (std::string line; std::getline(lines, line);)
		names.push_back(line);
	g_free(text);
	while (!names.empty() && names.back().empty())
		names.pop_back();
	if (!names.empty())
		s.desktop_names = names;
	s.desktop_popup_time = gtk_spin_button_get_value_as_int(ui->popup_time);

	s.edge_tiling = gtk_switch_get_active(ui->edge_tiling);
	s.edge_tiling_range = gtk_spin_button_get_value_as_int(ui->edge_range);

	for (Ui::Monitor &m : ui->monitors) {
		wb::OutputSetting out{m.name, m.kept};
		if (!gtk_switch_get_active(m.enabled))
			out.attrs["enabled"] = "no";
		auto put = [&out](const char *key, const std::string &v) {
			if (!v.empty())
				out.attrs[key] = v;
		};
		put("mode", selected_value(m.mode, m.modes));
		put("scale", selected_value(m.scale, m.scales));
		put("position", gtk_editable_get_text(GTK_EDITABLE(m.position)));
		put("transform", selected_value(m.transform, kTransforms));
		if (!out.attrs.empty())
			s.outputs.push_back(out);
	}
	for (const wb::OutputSetting &o : ui->other_outputs)
		s.outputs.push_back(o);

	std::vector<std::string> hdr;
	for (const Ui::Monitor &m : ui->monitors)
		if (ui->hdr.count(m.name) && gtk_check_button_get_active(ui->hdr.at(m.name)))
			hdr.push_back(m.name);
	for (const std::string &n : ui->hdr_others)
		hdr.push_back(n);
	std::vector<std::string> loaded = split_names(ui->hdr_loaded.value_or(""));
	std::vector<std::string> a = hdr, b = loaded;
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	if (a == b) {
		s.hdr_outputs = ui->hdr_loaded;  /* unchanged: as written */
	} else if (!hdr.empty()) {
		std::string joined;
		for (const std::string &n : hdr)
			joined += (joined.empty() ? "" : " ") + n;
		s.hdr_outputs = joined;
	}

	for (auto &[key, entry] : ui->xkb) {
		std::string v = gtk_editable_get_text(GTK_EDITABLE(entry));
		if (!v.empty())
			s.xkb[key] = v;
	}
	for (auto &[key, choice] : ui->libinput) {
		std::string v = selected_value(choice.dd, choice.values);
		if (!v.empty())
			s.libinput[key] = v;
	}
	s.xwayland = gtk_switch_get_active(ui->xwayland);
	s.virtual_input = selected_value(ui->virtual_input.dd, ui->virtual_input.values);
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

	/* Re-read the file (it may have changed since wbconf opened it) and
	 * write only what was changed here. */
	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	if (!doc) {
		set_status(ui, problem);
		return;
	}
	wb::WayboxSettings now = collect(ui);
	doc->apply(now, ui->shown);
	std::string err;
	if (!doc->save_file(path, &err)) {
		set_status(ui, std::string(_("Not saved: ")) + err);
		return;
	}
	ui->shown = now;
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
 * 4 Margins, 5 Menu, 6 Switcher, 7 Desktops, 8 Monitors, 9 Keyboard,
 * 10 Mouse and touchpad, 11 Advanced. */
void reset_page_widgets(Ui *ui, int page) {
	switch (page) {
	case 0:  /* Theme: clear the selection (built-in default theme). */
		gtk_list_box_unselect_all(ui->theme_list);
		break;
	case 1:  /* Appearance */
		gtk_spin_button_set_value(ui->pad_y, 2);
		gtk_spin_button_set_value(ui->button_size, 0);
		gtk_spin_button_set_value(ui->resize_grab, 8);
		gtk_editable_set_text(GTK_EDITABLE(ui->title_layout), "NLIMC");
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
		gtk_switch_set_active(ui->edge_tiling, TRUE);
		gtk_spin_button_set_value(ui->edge_range, 10);
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
		gtk_switch_set_active(ui->switcher_icons, TRUE);
		break;
	case 7:  /* Desktops */
		gtk_spin_button_set_value(ui->desktops_number, 4);
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(ui->desktop_names), "", -1);
		gtk_spin_button_set_value(ui->popup_time, 875);
		break;
	case 9:  /* Keyboard */
		for (auto &[key, entry] : ui->xkb)
			gtk_editable_set_text(GTK_EDITABLE(entry), "");
		break;
	case 10:  /* Mouse and touchpad */
		for (auto &[key, choice] : ui->libinput)
			gtk_drop_down_set_selected(choice.dd, 0);
		break;
	case 11:  /* Advanced */
		gtk_switch_set_active(ui->xwayland, TRUE);
		gtk_drop_down_set_selected(ui->virtual_input.dd, 0);
		break;
	case 8:  /* Monitors: everything automatic, no HDR */
		for (auto &[name, check] : ui->hdr)
			gtk_check_button_set_active(check, FALSE);
		for (Ui::Monitor &m : ui->monitors) {
			gtk_switch_set_active(m.enabled, TRUE);
			gtk_drop_down_set_selected(m.mode, 0);
			gtk_drop_down_set_selected(m.scale, 0);
			gtk_editable_set_text(GTK_EDITABLE(m.position), "");
			gtk_drop_down_set_selected(m.transform, 0);
		}
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

/* Closing with unsaved changes asks first. */
gboolean on_close_request(GtkWindow *window, gpointer data) {
	auto *ui = static_cast<Ui *>(data);
	if (!ui->loaded || collect(ui) == ui->shown)
		return FALSE;  /* close */
	GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", _("Discard unsaved changes?"));
	const char *buttons[] = {_("Cancel"), _("Discard"), nullptr};
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, window, nullptr,
			[](GObject *source, GAsyncResult *result, gpointer data) {
		auto *ui = static_cast<Ui *>(data);
		int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result,
				nullptr);
		if (button == 1) {
			ui->shown = collect(ui);  /* nothing left to lose */
			gtk_window_destroy(ui->window);
		}
	}, ui);
	g_object_unref(dialog);
	return TRUE;  /* keep it open until answered */
}

GtkWidget *build_theme_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Theme"));

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroller, TRUE);
	/* Room for several themes next to the preview. */
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 160);

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

	/* As obconf's "Button order". */
	grid_label(grid, 3, _("Button order:"));
	ui->title_layout = GTK_ENTRY(grid_control(grid, 3, gtk_entry_new()));
	gtk_entry_set_max_length(ui->title_layout, 16);
	GtkWidget *legend = gtk_label_new(_("N: Window icon\n"
			"L: Window label (title)\n"
			"I: Iconify\n"
			"M: Maximize\n"
			"C: Close\n"
			"S: Shade (roll up)\n"
			"D: Omnipresent (on all desktops)"));
	gtk_label_set_xalign(GTK_LABEL(legend), 0);
	gtk_widget_add_css_class(legend, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), legend, 1, 4, 1, 1);
	return page;
}

GtkWidget *build_windows_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *content = section(page, _("Placing Windows"));
	GtkWidget *grid = section_grid(content);
	ui->placement = add_dropdown(grid, 0, _("Prefer to place windows:"),
			kPlacement);

	GtkWidget *tiling = section_grid(section(page, _("Edge Tiling")));
	ui->edge_tiling = add_switch(tiling, 0, _("Tile windows dragged to a screen edge:"));
	ui->edge_range = add_spin(tiling, 1, _("Distance from the edge (px):"), 1, 100);
	return page;
}

GtkWidget *build_desktops_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("Desktops")));
	ui->desktops_number = add_spin(grid, 0, _("Number of desktops:"), 1, 64);
	ui->popup_time = add_spin(grid, 1, _("Show the name after switching (ms, 0 = off):"),
			0, 10000);
	gtk_spin_button_set_increments(ui->popup_time, 125, 500);

	GtkWidget *names = section(page, _("Names (one per line)"));
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 120);
	gtk_widget_set_vexpand(scroller, TRUE);
	GtkWidget *view = gtk_text_view_new();
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 4);
	ui->desktop_names = GTK_TEXT_VIEW(view);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	gtk_box_append(GTK_BOX(names), scroller);
	return page;
}

GtkWidget *build_monitors_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), inner);
	gtk_box_append(GTK_BOX(page), scroller);

	GtkWidget *note = gtk_label_new(_("Applied when a monitor is connected and on "
			"reload. While kanshi (or another output manager) runs, its profiles "
			"win; a monitor it changed keeps that for the session."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(inner), note);

	std::string why;
	auto monitors = query_monitors(&why);
	if (!monitors || monitors->empty()) {
		GtkWidget *msg = gtk_label_new(why.c_str());
		gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
		gtk_label_set_xalign(GTK_LABEL(msg), 0);
		gtk_box_append(GTK_BOX(inner), msg);
		return page;
	}
	ui->monitors.reserve(monitors->size());
	for (const MonitorInfo &info : *monitors) {
		std::string title = info.name;
		if (!info.description.empty())
			title += "  " + info.description;
		GtkWidget *grid = section_grid(section(inner, title.c_str()));
		Ui::Monitor m;
		m.name = info.name;
		m.enabled = add_switch(grid, 0, _("On:"));
		m.modes.push_back("");
		for (const std::string &v : info.modes)
			m.modes.push_back(v);
		grid_label(grid, 1, _("Mode:"));
		m.mode = values_dropdown(m.modes);
		grid_control(grid, 1, GTK_WIDGET(m.mode));
		m.scales = {"", "1", "1.25", "1.5", "1.75", "2", "2.5", "3"};
		grid_label(grid, 2, _("Scale:"));
		m.scale = values_dropdown(m.scales);
		grid_control(grid, 2, GTK_WIDGET(m.scale));
		grid_label(grid, 3, _("Position (x,y):"));
		m.position = GTK_ENTRY(grid_control(grid, 3, gtk_entry_new()));
		gtk_entry_set_placeholder_text(m.position, _("Automatic"));
		grid_label(grid, 4, _("Rotation:"));
		m.transform = values_dropdown(kTransforms);
		grid_control(grid, 4, GTK_WIDGET(m.transform));
		GtkWidget *hdr = gtk_check_button_new_with_label(
				_("HDR10 (Vulkan renderer and an HDR monitor)"));
		grid_control(grid, 5, hdr);
		ui->hdr[info.name] = GTK_CHECK_BUTTON(hdr);
		ui->monitors.push_back(std::move(m));
	}
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
	ui->switcher_icons = add_switch(grid, 3, _("Show window icons:"));
	return page;
}

GtkWidget *build_keyboard_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("Keyboard Layout")));
	struct Field { const char *key, *label, *hint; };
	const Field fields[] = {
		{"layout", _("Layouts:"), _("us  (several: us,de)")},
		{"variant", _("Variants:"), _("e.g. dvorak  (one per layout)")},
		{"options", _("Options:"), _("e.g. grp:alt_shift_toggle,caps:escape")},
		{"model", _("Model:"), _("e.g. pc105")},
	};
	int row = 0;
	for (const Field &f : fields) {
		grid_label(grid, row, f.label);
		GtkWidget *entry = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(entry), f.hint);
		gtk_widget_set_size_request(entry, 280, -1);
		grid_control(grid, row++, entry);
		ui->xkb[f.key] = GTK_ENTRY(entry);
	}
	GtkWidget *note = gtk_label_new(_("Empty fields use the system default. "
			"Options and layouts are XKB names, as listed in xkeyboard-config(7)."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);
	return page;
}

GtkWidget *build_mouse_page(Ui *ui) {
	GtkWidget *page = make_page();
	const std::vector<std::pair<std::string, std::string>> onoff = {
		{"", _("Default")}, {"enabled", _("On")}, {"disabled", _("Off")}};
	auto add = [ui](GtkWidget *grid, int row, const char *key, const char *label,
			const std::vector<std::pair<std::string, std::string>> &items) {
		grid_label(grid, row, label);
		Ui::Choice c;
		c.dd = choice_dropdown(items, c.values);
		grid_control(grid, row, GTK_WIDGET(c.dd));
		ui->libinput[key] = c;
	};
	std::vector<std::pair<std::string, std::string>> speeds = {{"", _("Default")}};
	for (int i = -4; i <= 4; i++) {
		char v[16];
		snprintf(v, sizeof(v), "%g", i / 4.0);
		speeds.push_back({v, i == 0 ? std::string("0") : std::string(v)});
	}

	GtkWidget *pointer = section_grid(section(page, _("Mouse and Touchpad")));
	add(pointer, 0, "accelProfile", _("Acceleration:"),
			{{"", _("Default")}, {"adaptive", _("Adaptive")}, {"flat", _("Flat")}});
	add(pointer, 1, "accelSpeed", _("Speed (-1 slow to 1 fast):"), speeds);
	add(pointer, 2, "naturalScroll", _("Natural scrolling:"), onoff);
	add(pointer, 3, "leftHanded", _("Left-handed:"), onoff);
	add(pointer, 4, "middleEmulation", _("Middle click by pressing both buttons:"), onoff);

	GtkWidget *touch = section_grid(section(page, _("Touchpad")));
	add(touch, 0, "tap", _("Tap to click:"), onoff);
	add(touch, 1, "tapDrag", _("Tap and drag:"), onoff);
	add(touch, 2, "tapDragLock", _("Drag lock:"), onoff);
	add(touch, 3, "disableWhileTyping", _("Disable while typing:"), onoff);
	add(touch, 4, "scrollMethod", _("Scrolling:"),
			{{"", _("Default")}, {"twofinger", _("Two fingers")}, {"edge", _("Edge")},
			 {"button", _("Holding a button")}, {"none", _("None")}});
	add(touch, 5, "clickMethod", _("Clicking:"),
			{{"", _("Default")}, {"buttonAreas", _("Button areas")},
			 {"clickfinger", _("Number of fingers")}, {"none", _("None")}});

	GtkWidget *note = gtk_label_new(_("Applied to every mouse and touchpad; a "
			"device ignores what it doesn't support. Default is the device's own."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
			GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page);
	gtk_widget_set_vexpand(scroller, TRUE);
	return scroller;
}

GtkWidget *build_advanced_page(Ui *ui) {
	GtkWidget *page = make_page();
	GtkWidget *grid = section_grid(section(page, _("X11 Programs")));
	ui->xwayland = add_switch(grid, 0, _("Run X11 programs (Xwayland):"));
	GtkWidget *vgrid = section_grid(section(page, _("Virtual Keyboards and Pointers")));
	grid_label(vgrid, 0, _("Accept their input:"));
	ui->virtual_input.dd = choice_dropdown(
			{{"unlocked", _("Unless the screen is locked (default)")},
			 {"always", _("Always")}, {"never", _("Never")}},
			ui->virtual_input.values);
	grid_control(vgrid, 0, GTK_WIDGET(ui->virtual_input.dd));
	GtkWidget *note = gtk_label_new(_("Virtual input comes from tools such as wtype, "
			"on-screen keyboards and remote-desktop programs."));
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_add_css_class(note, "dim-label");
	gtk_box_append(GTK_BOX(page), note);
	return page;
}

void add_tab(GtkWidget *notebook, GtkWidget *page, const char *label) {
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new(label));
}

void build_window(GtkApplication *app, Ui *ui) {
	ui->window = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(ui->window, _("Waybox Configuration Manager"));
	gtk_window_set_default_size(ui->window, 640, 560);

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
	/* A sidebar: twelve pages don't fit side by side. */
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_LEFT);
	gtk_box_append(GTK_BOX(vbox), notebook);
	ui->notebook = GTK_NOTEBOOK(notebook);

	add_tab(notebook, build_theme_page(ui), _("Theme"));
	add_tab(notebook, build_appearance_page(ui), _("Appearance"));
	add_tab(notebook, build_fonts_page(ui), _("Fonts"));
	add_tab(notebook, build_windows_page(ui), _("Windows"));
	add_tab(notebook, build_margins_page(ui), _("Margins"));
	add_tab(notebook, build_menu_page(ui), _("Menu"));
	add_tab(notebook, build_switcher_page(ui), _("Switcher"));
	add_tab(notebook, build_desktops_page(ui), _("Desktops"));
	add_tab(notebook, build_monitors_page(ui), _("Monitors"));
	add_tab(notebook, build_keyboard_page(ui), _("Keyboard"));
	add_tab(notebook, build_mouse_page(ui), _("Mouse"));
	add_tab(notebook, build_advanced_page(ui), _("Advanced"));

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
	ui->save = save;

	gtk_box_append(GTK_BOX(vbox), actions);

	std::string problem;
	std::optional<wb::RcDocument> doc = load_document(&problem);
	populate(ui, doc ? doc->read() : wb::WayboxSettings{});
	ui->loaded = doc.has_value();
	if (!ui->loaded) {
		/* A full-width banner above the tabs; Save stays off. */
		gtk_widget_set_sensitive(save, FALSE);
		GtkWidget *banner = gtk_label_new(problem.c_str());
		gtk_label_set_wrap(GTK_LABEL(banner), TRUE);
		gtk_label_set_xalign(GTK_LABEL(banner), 0);
		gtk_widget_add_css_class(banner, "error");
		gtk_widget_set_margin_top(banner, 8);
		gtk_widget_set_margin_start(banner, 12);
		gtk_widget_set_margin_end(banner, 12);
		gtk_box_prepend(GTK_BOX(vbox), banner);
	}
	g_signal_connect(ui->window, "close-request", G_CALLBACK(on_close_request), ui);
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
