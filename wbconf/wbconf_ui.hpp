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
#include "waybox/settings_schema.hpp"
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

	/* Theme (its own editor: a list with a live preview) */
	GtkListBox *theme_list = nullptr;
	std::vector<std::string> theme_names;    /* parallel to the list rows */

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
	std::set<std::string> listed_integrations;  /* ids rc.xml's <startup> uses */

	/* Live theme preview (redrawn when the theme/fonts change). */
	GtkWidget *preview = nullptr;

	/* The settings from the schema, one row each (see settings_view.cpp). */
	struct SettingRow {
		const wb::SettingDef *def = nullptr;
		std::string category;                 /* its category's title */
		std::optional<std::string> value;     /* now; nullopt: not set */
		std::optional<std::string> saved;     /* as rc.xml has it */
		GtkWidget *row = nullptr;             /* the whole row (search hides it) */
		GtkWidget *bar = nullptr;             /* the "modified" bar */
		GtkLabel *title = nullptr;            /* "Title", "Category: Title" when searching */
		GtkCheckButton *check = nullptr;
		GtkSpinButton *spin = nullptr;
		GtkDropDown *dropdown = nullptr;
		GtkEntry *entry = nullptr;
		GtkTextView *text = nullptr;
		FontPicker font;
		bool updating = false;                /* setting the control from code */
	};
	std::vector<wb::SettingCategory> schema;
	std::vector<std::unique_ptr<SettingRow>> rows;
	/* Each category's heading in the list and its table-of-contents row;
	 * section headings, hidden with their settings while searching. */
	struct CategoryView {
		std::string id;
		GtkWidget *heading = nullptr;
		GtkWidget *toc = nullptr;
		GtkWidget *description = nullptr;
		std::vector<std::pair<GtkWidget *, std::vector<GtkWidget *>>> sections;
	};
	std::vector<CategoryView> categories;
	GtkSearchEntry *search = nullptr;
	GtkListBox *toc = nullptr;
	GtkScrolledWindow *scroller = nullptr;
	GtkWidget *list = nullptr;
	GtkLabel *no_results = nullptr;
	bool scrolling_to = false;            /* ToC click: don't re-select on scroll */

	/* What the custom editors (theme, monitors, programs) showed after
	 * loading or the last save: Save writes only what differs. */
	wb::WayboxSettings shown;
	GtkWidget *save = nullptr;
	bool loaded = false;  /* false: rc.xml has an error; nothing is saved */
};

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
		const std::string &name, const std::string &command, bool titled);
void on_add_program(GtkButton *, gpointer data);
void populate_programs(Ui *ui, const std::vector<wb::StartupSetting> &startup);
std::vector<wb::StartupSetting> collect_programs(Ui *ui);
wb::WayboxSettings collect(Ui *ui);
void set_status(Ui *ui, const std::string &text);
void on_save(GtkButton *, gpointer data);
void on_close(GtkButton *, gpointer data);
void on_reset_all(GtkButton *, gpointer data);
gboolean on_close_request(GtkWindow *window, gpointer data);
void build_window(GtkApplication *app, Ui *ui);
void on_activate(GtkApplication *app, gpointer data);

/* The custom editors, each a block placed in its setting's row. */
GtkWidget *build_theme_block(Ui *ui);
GtkWidget *build_monitors_block(Ui *ui);
const char *integration_category_title(const std::string &category);
void load_integrations_data(Ui *ui, const wb::WayboxSettings &s);
std::vector<wb::SettingDef> program_settings(Ui *ui);
GtkWidget *build_program_block(Ui *ui, const std::string &id);
GtkWidget *build_other_programs_block(Ui *ui);
GtkWidget *make_font_picker(FontPicker *out, GCallback on_change, gpointer user);

/* settings_view.cpp: the searchable list of every setting */
GtkWidget *build_settings_view(Ui *ui);
void populate(Ui *ui, const wb::WayboxSettings &s, const wb::RcDocument *doc);
void populate_rows(Ui *ui, const wb::RcDocument *doc);
void rows_saved(Ui *ui);
/* WBCONF_TEST_SET: change settings as the window would (tests). */
void apply_test_values(Ui *ui, const char *spec);
bool rows_unsaved(Ui *ui);
void reset_all_rows(Ui *ui);
/* A setting's value now (or its default), by id. */
std::string setting_value(Ui *ui, const std::string &id);
void focus_category(Ui *ui, const std::string &category_id);
/* Save enabled and the status line counting changes, when anything differs
 * from rc.xml. */
void refresh_save_state(Ui *ui);

}  // namespace wbconf

#endif
