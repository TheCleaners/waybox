/*
 * wbconf's shared declarations: the widget state (Ui) and the functions
 * its source files share. See main.cpp for the overview.
 */
#ifndef WBCONF_UI_HPP
#define WBCONF_UI_HPP

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
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
#include "waybox/integrations.hpp"
#include "waybox/startup.hpp"
#include "waybox/theme.hpp"
#include "waybox/style.hpp"
#include "waybox/render.hpp"

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "waybox"
#endif
#define _(s) gettext(s)
#define N_(s) (s)

namespace wbconf {

namespace fs = std::filesystem;

/* ---- Monitors, as wlr-randr --json reports them ----------------------- */

struct MonitorInfo {
	std::string name;         /* connector */
	std::string description;  /* make, model */
	std::vector<std::string> modes;  /* "WxH@HZ" */
};

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

	/* Integrations: one row per known program (from the integration files)
	 * and per other program in <startup>. */
	struct ProgramRow {
		const wb::Integration *in = nullptr;  /* nullptr: a custom program */
		std::string name;                     /* its <startup> name */
		std::map<std::string, std::string> attrs;  /* as loaded (restart...) */
		bool listed = false;                  /* in <startup> when loaded */
		GtkSwitch *on = nullptr;
		GtkEntry *command = nullptr;
		GtkEntry *name_entry = nullptr;       /* custom programs */
		std::map<std::string, GtkEntry *> options;
		GtkWidget *box = nullptr;
		GtkWidget *details = nullptr;  /* options and command: shown when on */
		GtkLabel *status = nullptr;
		bool syncing = false;
		bool removed = false;
	};
	std::vector<wb::Integration> integrations;
	std::vector<std::unique_ptr<ProgramRow>> programs;
	GtkWidget *custom_box = nullptr;
	std::vector<std::string> in_autostart;  /* programs the autostart script runs */
	std::set<std::string> running;          /* process names running now */
	GtkSpinButton *screen_off = nullptr;    /* minutes; 0 never */

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

inline const char *const kPlacement[] = {"Smart", "Center", "UnderMouse", nullptr};
inline const char *const kSubmenu[] = {"hover", "click", nullptr};
inline const char *const kOrder[] = {"mru", "stacking", "spatial", nullptr};
inline std::vector<std::string> kTransforms = {"", "normal", "90", "180", "270", "flipped",
		"flipped-90", "flipped-180", "flipped-270"};

std::string home_dir();
std::string user_rc_path();
std::string system_rc_path();
std::optional<wb::RcDocument> load_document(std::string *problem);
pid_t session_compositor();
bool is_waybox(long pid);
int signal_waybox_reload();
std::string format_hz(double hz);
std::optional<std::vector<MonitorInfo>> query_monitors(std::string *why);
void populate_monitors(Ui *ui, const std::vector<wb::OutputSetting> &outputs);
GtkWidget *build_monitors_page(Ui *ui);
int index_of(const char *const *list, const std::string &value, int fallback);
GtkWidget *make_page();
GtkWidget *section(GtkWidget *page, const char *title);
GtkWidget *section_grid(GtkWidget *content);
void grid_label(GtkWidget *grid, int row, const char *text);
GtkWidget *grid_control(GtkWidget *grid, int row, GtkWidget *w);
GtkSpinButton *add_spin(GtkWidget *grid, int row, const char *label,
		int min, int max);
GtkSwitch *add_switch(GtkWidget *grid, int row, const char *label);
GtkDropDown *add_dropdown(GtkWidget *grid, int row, const char *label,
		const char *const *items);
GtkDropDown *values_dropdown(const std::vector<std::string> &values);
GtkDropDown *choice_dropdown(const std::vector<std::pair<std::string, std::string>> &items,
		std::vector<std::string> &values);
std::vector<std::string> split_names(const std::string &list);
void select_value(GtkDropDown *dd, std::vector<std::string> &values,
		const std::string &value);
std::string selected_value(GtkDropDown *dd, const std::vector<std::string> &values);
void add_font(GtkWidget *grid, int row, const char *label, FontPicker *out,
		GCallback on_change, gpointer user);
std::optional<std::string> font_value(const FontPicker *p);
void font_set(FontPicker *p, const std::optional<std::string> &v);
void to_cairo(cairo_t *cr, const wb::Color &c);
void preview_glyph(cairo_t *cr, double cx, double cy, double s, int kind,
		const wb::Color &fg);
int preview_titlebar(cairo_t *cr, const wb::Theme &theme, bool active,
		int x, int y, int w, const char *title);
void preview_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
		gpointer data);
void preview_refresh(Ui *ui);
GtkWidget *build_theme_page(Ui *ui);
std::set<std::string> running_processes();
bool is_running(Ui *ui, const std::string &binary);
bool installed(const std::string &binary);
std::string autostart_text();
void program_status(Ui *ui, Ui::ProgramRow *row);
void on_program_toggled(GObject *, GParamSpec *, gpointer data);
void on_option_changed(GtkEditable *, gpointer data);
void on_command_changed(GtkEditable *, gpointer data);
void on_remove_program(GtkButton *, gpointer data);
Ui::ProgramRow *add_program_row(Ui *ui, GtkWidget *parent, const wb::Integration *in,
		const std::string &name, const std::string &command);
void on_add_program(GtkButton *, gpointer data);
void populate_programs(Ui *ui, const std::vector<wb::StartupSetting> &startup);
std::vector<wb::StartupSetting> collect_programs(Ui *ui);
GtkWidget *build_integrations_page(Ui *ui);
GtkWidget *build_fonts_page(Ui *ui);
GtkWidget *build_appearance_page(Ui *ui);
GtkWidget *build_windows_page(Ui *ui);
GtkWidget *build_desktops_page(Ui *ui);
GtkWidget *build_margins_page(Ui *ui);
GtkWidget *build_menu_page(Ui *ui);
GtkWidget *build_switcher_page(Ui *ui);
GtkWidget *build_keyboard_page(Ui *ui);
GtkWidget *build_mouse_page(Ui *ui);
GtkWidget *build_advanced_page(Ui *ui);
void populate(Ui *ui, const wb::WayboxSettings &s);
wb::WayboxSettings collect(Ui *ui);
void reset_page_widgets(Ui *ui, int page);
void set_status(Ui *ui, const std::string &text);
void on_save(GtkButton *, gpointer data);
void on_close(GtkButton *, gpointer data);
void on_reset_page(GtkButton *, gpointer data);
void on_reset_all(GtkButton *, gpointer data);
gboolean on_close_request(GtkWindow *window, gpointer data);
void add_tab(GtkWidget *notebook, GtkWidget *page, const char *label);
void build_window(GtkApplication *app, Ui *ui);
void on_activate(GtkApplication *app, gpointer data);

}  // namespace wbconf

#endif
