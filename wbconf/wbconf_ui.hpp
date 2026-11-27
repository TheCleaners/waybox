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
#include <functional>
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

#include "waybox/binding_clash.hpp"
#include "waybox/font_spec.hpp"
#include "waybox/name_list.hpp"
#include "waybox/page_protocol.hpp"
#include "waybox/change_list.hpp"
#include "waybox/setting_problems.hpp"
#include "waybox/wbconf.hpp"
#include "waybox/settings_schema.hpp"
#include "waybox/integrations.hpp"
#include "waybox/startup.hpp"
#include "waybox/startup_sources.hpp"
#include "waybox/gtk_appearance.hpp"
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
	std::string make, model, serial;  /* each may be "" */
	std::vector<std::string> modes;  /* "WxH@HZ" */
	/* As it is now: what "Automatic" shows in the arranger. */
	bool enabled = true;
	int width = 0, height = 0;  /* the current mode */
	int x = 0, y = 0;
	double scale = 1;
	std::string transform = "normal";
	bool adaptive_sync = false;
	/* Its make, model and serial came as valid UTF-8, so a rule can match
	 * them exactly (else following it is off). */
	bool identity_ok = true;
};

/* ---- A lightweight font picker: family + size + bold ----------------- */

/* The installed font families, sorted (fonts.cpp). */
const std::vector<std::string> &font_families();


/* ---- Widget set, bound to a WayboxSettings on save ------------------- */

/* A color control (color_control.cpp): a tinted text field holding the
 * value, GTK's color button, and an eyedropper (Waybox's pick_color). */
struct ColorControl {
	GtkWidget *box = nullptr;
	GtkEntry *entry = nullptr;  /* the value: its "changed" is the owner's */
	GtkColorDialogButton *button = nullptr;
	GtkWidget *pick = nullptr;
	std::string css_class, default_value, hint;
	bool alpha = false, syncing = false, picking = false;
	std::shared_ptr<ColorControl *> self;  /* nulled when gone: a late pick */
	~ColorControl();
};
std::unique_ptr<ColorControl> make_color_control(const std::string &hint,
		const std::string &default_value, bool alpha);
/* After the field's text was set from elsewhere: tint and button. */
void color_control_refresh(ColorControl *c);

/* An ordered list of names (list_control.cpp): rows with Move Up, Move
 * Down and Remove; Add… with suggestions. */
struct ListControl {
	GtkWidget *box = nullptr, *rows = nullptr, *add = nullptr, *popover = nullptr;
	GtkEntry *entry = nullptr;
	GtkWidget *suggest_box = nullptr, *suggest_scroll = nullptr;
	wb::names::List items, suggestions;
	std::vector<std::string> suggestion_labels;
	std::function<void()> on_change;  /* the user changed it */
	bool syncing = false;
};
std::unique_ptr<ListControl> make_list_control(const std::vector<std::string> &suggestions,
		const std::vector<std::string> &labels, const std::string &hint);
/* Shown, without on_change. */
void list_control_set(ListControl *c, const wb::names::List &items);
bool list_control_test(ListControl *c, const std::string &action);

/* A font (font_control.cpp): a page's font { }. */
struct FontControl {
	GtkWidget *box = nullptr, *sample = nullptr;
	GtkDropDown *weight = nullptr, *width = nullptr;
	/* The family: a button saying it, opening a search, a Monospace
	 * toggle and the list, each name in its own font. */
	GtkWidget *family_button = nullptr, *family_label = nullptr, *family_pop = nullptr;
	GtkWidget *family_search = nullptr, *mono_toggle = nullptr, *family_view = nullptr;
	GtkStringList *family_model = nullptr;
	GtkFilterListModel *family_filtered = nullptr;
	GtkCustomFilter *family_filter = nullptr;
	guint family_index = 0;       /* in families: 0 Default */
	std::string default_label;    /* "Default (monospace 10)" */
	GtkSpinButton *size = nullptr;
	GtkSwitch *italic = nullptr;
	/* Weight, width and italic: in a popover, behind a button saying which. */
	GtkWidget *style = nullptr, *style_label = nullptr, *preview = nullptr;
	std::vector<std::string> families;  /* parallel to the list; "" (Default) first */
	bool fontconfig = false;            /* else as Pango writes it */
	double default_size = 11;           /* the default's (its hint's), shown then */
	/* What the weight and width lists offer: 0 (the font's own) first. */
	std::vector<int> weight_values, width_values;
	wb::font::Spec spec;
	PangoFontDescription *pango_desc = nullptr;  /* Pango: what was there */
	std::string value;
	std::function<void()> on_change;
	bool syncing = false;
	~FontControl();
};
/* `monospace`: the list starts showing monospace families only (a
 * terminal's font); the toggle shows the rest. */
std::unique_ptr<FontControl> make_font_control(const std::string &format,
		const std::string &hint, bool monospace = false);
void font_control_set(FontControl *c, const std::string &value);
bool font_control_test(FontControl *c, const std::string &action);

/* Repeated sections (sections_control.cpp): a page's sections { }. */
struct OptionField;
struct SectionsControl {
	GtkWidget *box = nullptr, *rows = nullptr, *add = nullptr, *popover = nullptr;
	GtkEntry *entry = nullptr;
	GtkWidget *suggest_box = nullptr, *suggest_scroll = nullptr;
	std::vector<wb::page::Section> items;
	std::vector<bool> expanded;
	std::vector<wb::page::Control> fields;  /* each section's */
	std::vector<std::string> suggestions, suggestion_labels;
	/* Shown now, a row at a time. */
	std::vector<std::vector<std::unique_ptr<wb::IntegrationOption>>> row_opts;
	std::vector<std::vector<std::unique_ptr<OptionField>>> row_fields;
	std::vector<GtkLabel *> summaries;
	std::vector<GtkEntry *> name_entries;
	std::function<void()> on_change;
	bool syncing = false;
	~SectionsControl();
};
std::unique_ptr<SectionsControl> make_sections_control(const wb::page::Control &control);
void sections_control_set(SectionsControl *c, const std::vector<wb::page::Section> &items);
bool sections_control_test(SectionsControl *c, const std::string &action);

/* One integration option's widget (option_widgets.cpp), by its Type=. */
struct OptionField {
	const wb::IntegrationOption *opt = nullptr;
	GtkWidget *widget = nullptr;   /* what goes in the grid */
	GtkEntry *entry = nullptr;     /* the value, for types that take text */
	GtkDropDown *drop = nullptr;   /* choice */
	GtkStringList *drop_model = nullptr;
	std::vector<std::string> drop_values;  /* "" first: the default */
	GtkSwitch *sw = nullptr;       /* switch */
	GtkPicture *thumb = nullptr;   /* images: the chosen one */
	GtkColorDialogButton *color = nullptr;
	std::unique_ptr<ColorControl> color_control;
	std::unique_ptr<ListControl> list;  /* list (pages only) */
	std::unique_ptr<SectionsControl> sections;  /* sections (pages only) */
	std::unique_ptr<FontControl> font;          /* font (pages only) */
	bool syncing = false;
	std::function<void()> on_change;  /* the user changed it */
};
std::unique_ptr<OptionField> make_option_field(const wb::IntegrationOption &o,
		std::function<void()> on_change);
std::string option_field_value(const OptionField &f);
/* Show `value` (from the command), without calling on_change. */
void set_option_field_value(OptionField &f, const std::string &value);

struct ShortcutEdit;  /* shortcuts_page.cpp */

struct Ui {
	/* Built and shown: a switch changing now is the user's (or a test's). */
	bool ready = false;
	/* WBCONF_TEST_CHOICE: "waybox", "waybox+stop" or "script", in place of
	 * asking when a program the script starts is turned on. */
	std::string test_choice;
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
		GtkDropDown *transform = nullptr;    /* rotation: "", normal, 90, 180, 270 */
		std::vector<std::string> rotations;
		GtkCheckButton *flip = nullptr;
		GtkDropDown *adaptive = nullptr;     /* Automatic, On, Off */
		std::string adaptive_raw;            /* as written, kept while unchanged */
		guint adaptive_initial = 0;
		GtkSwitch *follow = nullptr;         /* match make/model/serial, not the port */
		GtkLabel *matched_by = nullptr;
		std::map<std::string, std::string> kept;  /* attributes not shown here */
		/* The rule this card edits (the one Waybox applies to it), by the
		 * matcher it was read with; empty: none yet. */
		wb::OutputMatch rule_from;
		wb::OutputMatch match_initial;  /* its matcher as loaded */
		bool follow_initial = false;
		/* The rule applying to it applies to another connected monitor too:
		 * changed, it gets a rule of its own, before that one. */
		bool shared = false;
		wb::OutputMatch shared_match;
		std::map<std::string, std::string> shared_attrs;
		std::vector<std::string> shared_with;
		MonitorInfo info;
		GtkWidget *card = nullptr;      /* its settings, shown when selected */
		GtkLabel *looks_like = nullptr;  /* its size on the desktop */
	};
	std::vector<Monitor> monitors;
	/* The arranger (monitor_arranger.cpp): monitors as boxes to drag. */
	struct {
		GtkWidget *area = nullptr;
		GtkCheckButton *advanced = nullptr;  /* every monitor's settings */
		GtkWidget *identify = nullptr;
		GtkWidget *manager_notice = nullptr;  /* kanshi & co. running */
		GtkWidget *manager_stop = nullptr;    /* its Stop or Turn Off (tests) */
		int selected = -1;                   /* in monitors */
		int dragging = -1;
		int drag_x = 0, drag_y = 0;          /* where it's dragged to (logical) */
		int start_x = 0, start_y = 0;
		/* Canvas = (logical - origin) * scale + margin; kept while dragging. */
		double scale = 0.1, origin_x = 0, origin_y = 0, margin_x = 0, margin_y = 0;
	} arranger;
	/* Rules for monitors that aren't connected now: passed through as is. */
	std::vector<wb::OutputSetting> other_outputs;

	/* HDR: per-monitor checks; names in <hdr outputs> for monitors that
	 * aren't connected; and the attribute as loaded (kept as written when
	 * the set of names doesn't change). */
	std::map<std::string, GtkCheckButton *> hdr;
	std::vector<std::string> hdr_others;
	std::optional<std::string> hdr_loaded;

	/* GTK programs' appearance (gtk_page.cpp): the themes they use, shared
	 * with other desktops through GSettings and settings.ini. */
	struct GtkLook {
		std::string icons, theme, cursor, scheme;  /* scheme: default/prefer-dark/... */
		int size = 24;
		bool operator==(const GtkLook &) const = default;
	};
	struct {
		std::map<std::string, wb::IconTheme> icon_themes;
		std::vector<std::string> icon_values, theme_values, cursor_values, scheme_values;
		GtkDropDown *icons = nullptr, *theme = nullptr, *cursor = nullptr, *scheme = nullptr;
		GtkSpinButton *size = nullptr;
		GtkLabel *health = nullptr;
		GtkWidget *fill = nullptr, *undo = nullptr;
		GtkLook saved;
		bool built = false;
		/* The pointer's fix chosen: Save writes the theme shown to rc.xml
		 * (Waybox's pointer) even if nothing else changed. */
		bool pointer_to_rc = false;
		/* rc.xml's <cursor>, which the pointer controls show (and write) for
		 * every program, not only GTK's: applied whichever of the page and
		 * the settings comes first. */
		std::optional<std::string> rc_cursor_theme, rc_cursor_size;
	} gtk;

	/* Programs started from elsewhere: the autostart script's start lines
	 * and XDG autostart entries, each switchable (startup_sources_page.cpp). */
	struct SourceRow {
		bool script = true;        /* else an XDG autostart entry */
		int line = 0;              /* script: its line */
		std::string command;       /* script: the line as read */
		std::string file, path;    /* XDG: the .desktop's name, the file that counts */
		std::string program;
		bool saved = true;         /* on, as the files have it */
		bool added = false;        /* appeared while wbconf ran */
		GtkSwitch *on = nullptr;
		GtkLabel *status = nullptr;
	};
	std::vector<std::unique_ptr<SourceRow>> source_rows;
	std::string script_path;       /* the script the session runs */
	bool script_is_user = false;   /* in the user's config (else the default) */
	std::vector<wb::ScriptEntry> script_entries;
	std::vector<wb::AutostartEntry> xdg_entries;
	/* Watching them: the files changing on disk refill the lists, keeping
	 * switches not saved yet, with a note saying so. */
	GtkWidget *script_rows = nullptr, *xdg_rows = nullptr;      /* the blocks' rows */
	GtkWidget *script_note = nullptr, *xdg_note = nullptr;      /* "changed outside..." */
	std::vector<GFileMonitor *> source_monitors;
	guint source_refill = 0;  /* the debounce timeout */
	/* Said after a save's own message ("Saved and reloaded waybox."): what
	 * the programs' pages want known (restart one to see its settings). */
	std::vector<std::string> save_notes;

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
		std::map<std::string, std::unique_ptr<OptionField>> options;
		GtkWidget *box = nullptr;
		GtkWidget *details = nullptr;  /* options and command: shown when on */
		GtkLabel *status = nullptr;
		GtkWidget *configure = nullptr;  /* Configure=: its settings tool */
		/* Page=: wbconf's page for it (pages/), and how it's told the
		 * options changed. */
		std::shared_ptr<void> page;
		std::function<void()> page_refresh;
		/* "NAME Settings": its typed options or built-in page, folded or
		 * not as "Keep Program Settings Open" says (a Lua page folds its
		 * own). */
		GtkWidget *settings = nullptr;
		bool syncing = false;
		bool removed = false;
		/* Turned on while the autostart script starts it too: asked
		 * once; with "stop it", its running copies go when saving. */
		bool asked = false;
		bool stop_on_save = false;
		std::vector<pid_t> stop_pids;  /* the copies shown when asked */
		bool via_function = false;  /* the script starts it from a function */
	};
	std::vector<wb::Integration> integrations;
	/* Installed programs' versions (Version= in their integration file),
	 * by binary, as they come in; "" while asked or if none was printed. */
	std::map<std::string, std::string> versions;
	std::vector<std::unique_ptr<ProgramRow>> programs;
	GtkWidget *custom_box = nullptr;
	std::vector<std::string> in_autostart;  /* programs the autostart script runs */
	std::set<std::string> running;          /* process names running now */
	std::set<std::string> listed_integrations;  /* ids rc.xml's <startup> uses */

	/* Programs run from a key (Kind=binding: emoji, clipboard pickers): an
	 * Execute key binding each. */
	struct BindingRow {
		const wb::Integration *in = nullptr;
		std::optional<std::pair<std::string, std::string>> saved;  /* key, command */
		GtkSwitch *on = nullptr;
		GtkEntry *key = nullptr;
		GtkEntry *command = nullptr;
		GtkLabel *status = nullptr;
		GtkWidget *details = nullptr;
		std::shared_ptr<void> page;  /* its Lua page, if it has one */
	};
	std::vector<std::unique_ptr<BindingRow>> bindings;
	/* Every Lua page shown (its row owns it): saved, listed, tested. */
	std::vector<std::weak_ptr<void>> lua_pages;
	/* The Shortcuts page's two lists. */
	GtkWidget *shortcut_keys = nullptr;
	GtkWidget *shortcut_mouse = nullptr;
	GtkWidget *shortcut_add = nullptr;
	GtkWidget *shortcut_mouse_add = nullptr;
	GtkWidget *shortcut_suggest = nullptr;  /* "Add a shortcut for ..." */
	GtkSizeGroup *shortcut_column = nullptr;  /* the keys' column */
	GtkSizeGroup *shortcut_mouse_column = nullptr;
	/* Each key binding as saved and as it is now (shared_ptr: the type is
	 * the page's own). */
	std::vector<std::shared_ptr<ShortcutEdit>> shortcut_edits;
	ShortcutEdit *capturing = nullptr;  /* whose keys are being pressed */
	/* The window rules list, and its rows as text (tests). */
	GtkWidget *rules_box = nullptr;
	std::vector<std::string> rule_lines;
	/* Keys scripts bind (init.lua and rc.xml's <scripts>), read at load. */
	std::vector<wb::ScriptKey> script_keys;
	GtkEventController *capture_keys = nullptr;
	std::vector<std::string> bound_keys;  /* every key rc.xml binds */

	/* The input method: None, an installed framework (from the integration
	 * files), or Custom; with its command and variables. */
	struct {
		GtkDropDown *choice = nullptr;
		std::vector<const wb::Integration *> frameworks;  /* choices 1..n */
		GtkEntry *command = nullptr;
		GtkTextView *env = nullptr;   /* NAME=VALUE, one per line */
		GtkWidget *details = nullptr;
		GtkWidget *configure = nullptr;  /* the framework's settings tool */
		bool updating = false;
	} im;

	/* Live theme preview (redrawn when the theme/fonts change), and a
	 * thumbnail per theme in the list, as ObConf has. */
	GtkWidget *preview = nullptr;
	std::map<std::string, wb::Theme> theme_cache;
	std::vector<GtkWidget *> theme_thumbs;

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
		std::unique_ptr<ColorControl> color;  /* type color */
		std::unique_ptr<FontControl> font;
		bool updating = false;                /* setting the control from code */
		/* A problem found with it: an icon by its title, a note under it. */
		std::optional<wb::SettingProblem> problem;
		GtkWidget *problem_icon = nullptr;
		GtkWidget *problem_note = nullptr;
		GtkLabel *problem_text = nullptr;
		GtkWidget *problem_fix = nullptr;
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
		/* In the sidebar: its arrow, and whether it's open (by the user,
		 * or for now because it's what's being read or searched). */
		GtkImage *arrow = nullptr;
		bool open_by_user = false, open_for_now = false;
		GtkWidget *problem_icon = nullptr;  /* a setting in it has a problem */
	};
	std::vector<CategoryView> categories;
	/* The sidebar's rows, a tree: each category, then its sections. */
	struct TocEntry {
		GtkWidget *row = nullptr;
		int category = 0;
		GtkWidget *heading = nullptr;  /* what it scrolls to */
		bool section = false;
		GtkWidget *problem_icon = nullptr;  /* a setting in it has a problem */
	};
	std::vector<TocEntry> toc_entries;
	GtkSearchEntry *search = nullptr;
	GtkListBox *toc = nullptr;
	GtkScrolledWindow *scroller = nullptr;
	GtkWidget *list = nullptr;
	GtkLabel *no_results = nullptr;
	bool scrolling_to = false;            /* ToC click: don't re-select on scroll */
	/* A sidebar click's heading, scrolled to again once things above it
	 * have their sizes (images, statuses), unless the user scrolled since. */
	GtkWidget *scroll_target = nullptr;
	double scroll_set = -1;

	/* What the custom editors (theme, monitors, programs) showed after
	 * loading or the last save: Save writes only what differs. */
	wb::WayboxSettings shown;
	GtkWidget *save = nullptr;
	/* Beside the status: what's not saved yet, listed (hover, click). */
	GtkWidget *changes_button = nullptr;
	GtkWidget *changes_popover = nullptr;
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
/* One request to Waybox's socket; its answer, or nullptr after
 * `timeout_s` seconds or with no socket. */
JsonObject *waybox_request(const std::string &request, int timeout_s = 1);
/* The monitor arranger (monitor_arranger.cpp). */
GtkWidget *build_arranger(Ui *ui);
void arranger_changed(Ui *ui);   /* a monitor's settings changed: redraw */
/* For tests: the sidebar driven step by step (WBCONF_TEST_TOC). */
void start_toc_script(Ui *ui, const char *spec);
void arranger_select(Ui *ui, int index);
/* For tests: drop monitor NAME at logical x,y, as a drag would. */
bool arranger_test_drop(Ui *ui, const std::string &name, int x, int y);
/* For tests and screenshots: a monitor's make, model or description. */
void arranger_test_describe(Ui *ui, const std::string &what, const std::string &name,
		const std::string &text);
/* After saving changed monitor settings: keep them, or (unanswered) put
 * back the <outputs> and <hdr> elements as they were (snapshots). */
void ask_keep_monitors(Ui *ui, const std::string &outputs_xml, const std::string &hdr_xml);
void populate_hdr(Ui *ui, const std::optional<std::string> &hdr_outputs);
/* The notice above the monitors: a program that sets monitors itself
 * (Manages=outputs) is running, so its profiles win on a hotplug. */
void refresh_output_manager_notice(Ui *ui);
bool output_manager_test_stop(Ui *ui);
/* A monitor's card: its transform (rotation and flip, or one written by
 * hand), and the <output> rule it gives (nullopt: none to write). */
std::string monitor_transform(const Ui::Monitor &m);
std::optional<wb::OutputSetting> monitor_rule(const Ui::Monitor &m);
/* For tests: "monitor.NAME.KEY=VALUE" (rotation, flip, adaptive, follow,
 * scale), as the card's controls would. */
bool monitor_test_set(Ui *ui, const std::string &name, const std::string &key,
		const std::string &value);
std::string running_waybox_version();
std::string format_hz(double hz);
std::optional<std::vector<MonitorInfo>> query_monitors(std::string *why);
void populate_monitors(Ui *ui, const std::vector<wb::OutputSetting> &outputs);
int index_of(const char *const *list, const std::string &value, int fallback);
GtkWidget *make_page();
GtkWidget *section(GtkWidget *page, const char *title);
GtkWidget *section_grid(GtkWidget *content);
GtkWidget *grid_label(GtkWidget *grid, int row, const char *text);
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
void to_cairo(cairo_t *cr, const wb::Color &c);
void preview_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
		gpointer data);
void preview_refresh(Ui *ui);
void thumb_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data);
struct ProcessInfo {
	pid_t pid;
	uid_t uid;
	std::string name;  /* at most 15 characters */
	pid_t ppid = 0;
};
std::vector<ProcessInfo> list_processes();
std::string process_name(pid_t pid);
/* Its arguments (argv), or none if it's gone or not ours to read. */
std::vector<std::string> process_args(pid_t pid);
/* A variable of its environment: nullopt if unreadable, "" if unset. */
std::optional<std::string> process_env(pid_t pid, const std::string &name);
/* It runs in this Wayland session (WAYLAND_DISPLAY, XDG_RUNTIME_DIR as
 * ours), not another one of the user's. */
bool in_this_session(pid_t pid);
std::set<std::string> running_processes();
bool is_running(Ui *ui, const std::string &binary);
bool installed(const std::string &binary);

/* Integration pages (Page= in an integration file), in pages/: built into
 * a program's row, from its options; nullptr if wbconf has no such page,
 * or it doesn't suit the options. */
GtkWidget *build_integration_page(Ui *ui, Ui::ProgramRow *row);
GtkWidget *build_wallpaper_page(Ui *ui, Ui::ProgramRow *row);
/* Page=NAME.lua (lua_page.cpp): run by wbconf-lua, drawn here; its
 * changes listed and saved with the rest. */
GtkWidget *build_lua_page(Ui *ui, Ui::ProgramRow *row);
/* A key-bound program's page (owned by `holder`). */
/* A page's command line changed: it opens again for it (if nothing's unsaved). */
void lua_page_command_changed(const std::shared_ptr<void> &page);
/* Pages folded or kept open (the menu's "Keep Program Settings Open"),
 * remembered in $XDG_CONFIG_HOME/waybox/wbconf-view; setting it opens or
 * folds those shown. */
bool lua_pages_open();
/* wbconf's own view settings ($XDG_CONFIG_HOME/waybox/wbconf-view, one
 * KEY=VALUE a line): "" when not set. */
std::string view_get(const std::string &key);
void view_set(const std::string &key, const std::string &value);
std::string lua_page_fold(Ui *ui, const std::string &id);  /* tests: open|folded|hidden */
void set_lua_pages_open(Ui *ui, bool open);
/* `command`: the program's command line as its row has it now (its options
 * given to the page, wb.option), if it has one. */
GtkWidget *build_lua_page_for(Ui *ui, const wb::Integration *in, std::shared_ptr<void> *holder,
		std::function<std::string()> command = {}, std::function<bool()> in_use = {});
struct UnsavedChange;
void lua_page_changes(Ui *ui, const std::string &id, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out);
bool save_lua_pages(Ui *ui, std::string *error);
/* Tests: set a control ("KEY", value) or click ("click", KEY); dump it. */
void lua_page_test(Ui *ui, const std::string &id, const std::string &what,
		const std::string &value);
std::string lua_page_dump(Ui *ui, const std::string &id);
void wallpaper_refresh(void *page);
bool wallpaper_test_choose(void *page, const std::string &what);
std::string wallpaper_test_grid(void *page);  /* the grid's images, by name */
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
/* Saving: the running copies of programs moved from the autostart script
 * that the user chose to stop (before Waybox reloads and starts its own). */
void stop_moved_programs(Ui *ui);
wb::WayboxSettings collect(Ui *ui);
/* What saving compares against (ui->shown): collect(), but a <cursor> in
 * rc.xml as it is, not as shown (GTK's theme where it has a size only), so
 * a save completes it to what's shown. */
wb::WayboxSettings shown_settings(Ui *ui);
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
GtkWidget *build_binding_block(Ui *ui, const std::string &id);
GtkWidget *build_input_method_block(Ui *ui);
void populate_input_method(Ui *ui, const std::optional<wb::InputMethodSetting> &im);
std::optional<wb::InputMethodSetting> collect_input_method(Ui *ui);
/* Choose by integration id ("" none, "custom"), as the dropdown would. */
void choose_input_method(Ui *ui, const std::string &id);
void populate_bindings(Ui *ui, const wb::RcDocument *doc);
/* The Shortcuts page (shortcuts_page.cpp). */
GtkWidget *build_keybindings_block(Ui *ui);
GtkWidget *build_mousebindings_block(Ui *ui);
void populate_shortcuts(Ui *ui, const wb::RcDocument *doc);
/* Window rules (rules_page.cpp). */
GtkWidget *build_rules_block(Ui *ui);
void populate_rules(Ui *ui, const wb::RcDocument *doc);
/* The bindings changed, into rc.xml (at Save & Apply). */
void apply_shortcuts(Ui *ui, wb::RcDocument &doc);
/* Search: the "keybindings"/"mousebindings" list's rows matching `q`
 * shown (all when `whole`, the setting itself matching); whether
 * anything is. */
bool shortcuts_filter(Ui *ui, const std::string &widget, const std::string &q, bool setting_hit);
/* For search's @modified and @problems: unsaved rows; rows that never run. */
bool shortcuts_dirty(Ui *ui, const std::string &widget);
bool shortcuts_problem(Ui *ui, const std::string &widget);
/* Test hooks: the rows to stderr; a step ("shortcut-add:W-x=Close"). */
void shortcut_dump(Ui *ui);
void shortcut_test_step(Ui *ui, const std::string &step);
/* The binding rows' changes into `doc`; then what's saved is what's shown. */
void apply_bindings(Ui *ui, wb::RcDocument &doc);
void bindings_saved(Ui *ui);
int bindings_unsaved(Ui *ui);
bool binding_set(Ui *ui, const std::string &id);

/* settings_view.cpp: the searchable list of every setting */
GtkWidget *build_settings_view(Ui *ui);
void populate(Ui *ui, const wb::WayboxSettings &s, const wb::RcDocument *doc);
void populate_rows(Ui *ui, const wb::RcDocument *doc);
void rows_saved(Ui *ui);
/* WBCONF_TEST_SET: change settings as the window would (tests). */
void apply_test_values(Ui *ui, const char *spec);
wb::SettingDef gtk_appearance_setting();
GtkWidget *build_gtk_block(Ui *ui);
int gtk_unsaved(Ui *ui);
bool save_gtk(Ui *ui, std::string *error);
void gtk_test_value(Ui *ui, const std::string &key, const std::string &value);
void load_startup_sources(Ui *ui);
bool write_file_atomic(const std::string &path, const std::string &text, std::string *error);
std::vector<wb::SettingDef> startup_source_settings(Ui *ui);
GtkWidget *build_script_block(Ui *ui);
GtkWidget *build_xdg_block(Ui *ui);
/* Watch the autostart script and folders (once the blocks are built). */
void watch_startup_sources(Ui *ui);
/* Refill the lists from the files now, keeping switches not saved yet;
 * `outside`: the files changed outside wbconf (say so). */
void refill_startup_sources(Ui *ui, bool outside);
/* "sources-dump:script|LINE:PROGRAM=on|off[*][+]...|note:...": tests. */
std::string sources_dump(Ui *ui);
int sources_unsaved(Ui *ui);
bool save_sources(Ui *ui, std::string *error);
void sources_saved(Ui *ui);
void source_test_value(Ui *ui, const std::string &key, const std::string &value);
bool rows_unsaved(Ui *ui);
void reset_all_rows(Ui *ui);
/* A setting's value now (or its default), by id. */
std::string setting_value(Ui *ui, const std::string &id);
/* The value set now, or nullopt when unset (so automatic, not the default). */
std::optional<std::string> setting_if_set(Ui *ui, const std::string &id);
void focus_category(Ui *ui, const std::string &category_id);
/* Save enabled and the status line counting changes, when anything differs
 * from rc.xml. */
void refresh_save_state(Ui *ui);

/* A change not saved yet: how it reads in the list, and the widget to
 * scroll to for it (nullptr: none). Recomputed whenever needed: a reload
 * may destroy the widgets. */
struct UnsavedChange {
	wb::ChangeItem item;
	GtkWidget *target = nullptr;
};
/* Every change not saved yet, in the order of the page. */
std::vector<UnsavedChange> unsaved_changes(Ui *ui);
/* The custom editors' own: each change under `where` (its row's place
 * in the page) and `target`, the setting's row. */
void gtk_changes(Ui *ui, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out);
void sources_changes(Ui *ui, bool script, const std::string &where, GtkWidget *target,
		std::vector<UnsavedChange> &out);
void shortcut_changes(Ui *ui, const std::string &widget, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out);
void binding_changes(Ui *ui, const std::string &id, const std::string &where,
		GtkWidget *target, std::vector<UnsavedChange> &out);
/* The list button beside the status line, and bringing it up to date. */
GtkWidget *build_changes_button(Ui *ui);
void refresh_changes_button(Ui *ui, const std::vector<UnsavedChange> &changes);
void open_changes(Ui *ui);
/* Check the settings for problems again: each row's note, the sidebar's
 * marks. (From refresh_save_state: after every change, load and save.) */
void refresh_problems(Ui *ui);
/* A problem note's fix button. */
void on_problem_fix(GtkButton *button, gpointer data);
/* The GTK page's settings as shown now. */
Ui::GtkLook gtk_look_now(Ui *ui);
/* The pointer check's inputs, as they are now. */
wb::PointerState pointer_state(Ui *ui);
/* Scroll the settings to `w` (clearing a search that hides it). */
void reveal_in_list(Ui *ui, GtkWidget *w);

}  // namespace wbconf

#endif
