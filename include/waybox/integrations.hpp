#ifndef WB_INTEGRATIONS_HPP
#define WB_INTEGRATIONS_HPP

/*
 * Integrations: companion programs (panels, notification daemons, night
 * light...) described by small key-value files, so wbconf can offer them
 * and their options without knowing each one (#153). Pure; unit-tested.
 *
 *   [Integration]
 *   Name=wlsunset
 *   Category=NightLight
 *   Comment=Warmer colours at night
 *   Exec=wlsunset -l 51.5 -L -0.1
 *   Binary=wlsunset
 *
 *   Configure=wlsunset-gui (its own settings tool, if it has one)
 *   Page=wallpaper        (a page of wbconf's for it, if there's one)
 *   Manages=outputs       (it sets monitors itself, as kanshi: wbconf says
 *                          so while it runs)
 *
 *   Kind=binding          (run from a key, not at startup)
 *   Key=W-period          (the key it suggests)
 *
 *   [Option latitude]
 *   Label=Latitude
 *   Flag=-l
 *   Type=number           (wbconf's widget for it; text if not given)
 *   Min=-90
 *   Max=90
 *   Default=0             (what the program does without it; shown only)
 *
 * Types: text, file, folder, image, image-folder, image-or-folder, color,
 * choice (with Choices=a,b,c), number (Min=, Max=), and switch: a flag
 * with no value, there or not.
 *
 * The format is public (waybox-integration(5)): FormatVersion=MAJOR[.MINOR]
 * in [Integration], 1 if left out. A file of a newer major version is
 * skipped (not misread); a newer minor one is read, its new keys ignored.
 */
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

enum class OptionType {
	Text, File, Folder, Image, ImageFolder, ImageOrFolder, Color, Choice, Number, Switch,
};

struct IntegrationOption {
	std::string id;      /* from [Option id] */
	std::string label;
	std::string flag;    /* "-l": the value is the next word (none for a switch) */
	std::string hint;    /* example value, shown when empty */
	OptionType type = OptionType::Text;
	std::vector<std::string> choices;  /* Choice */
	std::optional<double> min, max;    /* Number */
	/* Default=: what the program does with the option left out
	 * (swaybg's mode: stretch), shown as such; never written. */
	std::string default_value;
	/* For pages (wbconf's Lua pages), not read from integration files:
	 * each choice's label (in step with `choices`; empty: the value),
	 * no "Default" entry (a page's choice always has a value), the unit
	 * after a number, a text field's width in characters (0: the row's). */
	std::vector<std::string> choice_labels;
	bool no_default_choice = false;
	std::string unit;
	int width_chars = 0;
	bool color_alpha = false;  /* a color with transparency (#rrggbbaa) */
	bool operator==(const IntegrationOption &) const = default;
};

/* Type= names ("image-folder"); Text for an unknown name. */
OptionType option_type_from_name(std::string_view name);

/* Whether `value` suits the option: a number in range, one of the
 * choices, "on" for a switch. Empty (not given) always does; the other
 * types take any text (a path may be "~/x" or "$VAR"). */
bool option_value_valid(const IntegrationOption &o, std::string_view value);

/* Reload=: how the program takes its settings again once a page saved
 * them (the page's wb.reload()). */
struct IntegrationReload {
	enum class Kind { Unset, None, Command, Signal };
	Kind kind = Kind::Unset;
	std::vector<std::string> argv;  /* command: what to run, as written */
	std::string signal;             /* signal: USR1, USR2 or HUP */
	bool operator==(const IntegrationReload &) const = default;
};

struct Integration {
	std::string id;        /* the file's name without .integration */
	std::string name;
	std::string category;  /* Panel, Notifications, Wallpaper, NightLight... */
	std::string comment;
	std::string exec;      /* the default command */
	std::string binary;    /* to detect it and match commands; Exec's program */
	/* Kind=binding: a program run from a key (an emoji or clipboard
	 * picker), not started with the session. Key= suggests the key, in
	 * rc.xml's syntax ("W-period" is Super+.). */
	bool binding = false;
	std::string key;
	/* Kind=inputmethod: an input method (IBus, Fcitx5); one at a time.
	 * Env=NAME=VALUE lines: the variables programs need to use it. */
	bool input_method = false;
	std::vector<std::pair<std::string, std::string>> env;
	std::vector<IntegrationOption> options;
	/* Version=: the command that prints its version (swaybg -v), when
	 * the program has one; never guessed, as a program that doesn't
	 * know the flag may just start. */
	std::string version_command;
	/* Configure=: the program's own settings tool (azote, waypaper,
	 * blueman-manager), which wbconf offers to open. */
	std::string configure;
	/* Page=: a page wbconf has for this kind of program ("wallpaper"),
	 * shown with its options; where wbconf has none, just the options. */
	std::string page;
	/* Manages=outputs: it sets monitors itself (kanshi, shikane): while
	 * it runs, its profiles win over Waybox's <outputs> on a hotplug. */
	bool manages_outputs = false;
	/* Page=NAME.lua: a settings page written in Lua (wbconf-lua), beside
	 * the file; Files= the files it may read and write ("~/..." allowed),
	 * Runs= the programs it may run, both comma-separated. */
	std::vector<std::string> files, runs;
	IntegrationReload reload;
	/* The directory the file was read from (load_integrations): where a
	 * Lua page is looked for. */
	std::string dir;
	bool operator==(const Integration &) const = default;
};

/* The version in a program's version output ("swaybg version 1.2.2",
 * "Waybar v0.15.0", "ibus-daemon - Version 1.5.35"): the first word that
 * is a version number, without a leading "v"; "" if none. */
std::string version_from_output(std::string_view output);

/* The format version read here: a file whose FormatVersion's major part
 * is greater is skipped. */
inline constexpr int kIntegrationFormat = 1;

/* What's wrong with a file, by line (0: the file as a whole). Errors make
 * it unusable (no Name or Exec, a newer major version); warnings are what
 * is ignored or guessed (an unknown key, an option without a Flag). */
struct IntegrationProblem {
	int line = 0;
	bool error = false;
	std::string message;
	bool operator==(const IntegrationProblem &) const = default;
};

struct IntegrationCheck {
	std::optional<Integration> integration;  /* nullopt: not usable */
	std::vector<IntegrationProblem> problems;
};

/* Read one file's text, saying what's wrong with it (wbconf
 * --check-integration prints this). */
IntegrationCheck check_integration(std::string_view id, std::string_view text);

/* Parse one file's text; nullopt without an [Integration] Name and Exec,
 * or of a newer format. */
std::optional<Integration> parse_integration(std::string_view id, std::string_view text);

/* A Lua page's script (Page=NAME.lua, beside the file): its path, or ""
 * for a built-in page or none. */
std::string lua_page_path(const Integration &in);

/* What a Lua page's Runs= and Files= amount to: full access to the account
 * when a program can run any command (a shell, an interpreter, env, sudo,
 * find, an editor...) or a file runs something later (a shell's startup
 * file, Waybox's autostart or rc.xml, ~/.local/bin...). The reason, said
 * after the program or file ("it can run any command"); "" if neither.
 * `home` makes absolute paths under it count as "~/..." too. */
std::string full_access_run(const std::string &program);
std::string full_access_file(const std::string &path, const std::string &home = "");

/* How far what a page may touch reaches. A list can't tell which programs
 * are dangerous (any can start others), so only what's plainly the
 * integration's own counts as routine:
 * - Own: its program (Binary=) or a companion named after it: the name
 *   and ctl, msg, client or cli, with or without a '-' (makoctl for mako;
 *   not any name it starts: a page names its program itself); a file in a
 *   folder named after it (~/.config/mako/config).
 * - Other: anything else: another program, with the account's access, or
 *   another program's file.
 * - Full: known to be full access (full_access_run, full_access_file). */
enum class Reach { Own, Other, Full };
struct PageAccess {
	Reach reach = Reach::Other;
	std::string why;  /* Full: the reason */
};
PageAccess run_access(const Integration &in, const std::string &program);
PageAccess file_access(const Integration &in, const std::string &path,
		const std::string &home = "");

/* Every *.integration in `dirs`, the first found of each id winning (pass
 * the user's directory first). Sorted by category, then name. */
std::vector<Integration> load_integrations(const std::vector<std::string> &dirs);

/* The directories to read, user's first: $XDG_CONFIG_HOME/waybox/integrations
 * (beside rc.xml, so installing Waybox into ~/.local can't overwrite them),
 * then each $XDG_DATA_DIRS entry's, then `installed` (the build's). */
std::vector<std::string> integration_dirs(const std::string &home,
		const std::string &xdg_config_home, const std::string &xdg_data_dirs,
		const std::string &installed);

/* Shell words of a command (spaces, '...' and "..." quotes, backslashes),
 * and back (quoting what needs it). Enough for the commands in rc.xml;
 * not a full shell. */
std::vector<std::string> split_command(std::string_view command);
std::string join_command(const std::vector<std::string> &words);

/* The options' values found in `command` (by flag), and a command with
 * `values` set: a changed value replaces the old one, an empty one drops
 * the flag, a new one is appended; everything else is kept as it was.
 * A switch's value is "on" when its flag is there; any value but "" adds
 * it (alone). */
std::map<std::string, std::string> read_options(const Integration &in,
		std::string_view command);
std::string write_options(const Integration &in, std::string_view command,
		const std::map<std::string, std::string> &values);

/* Which integration a startup program is: by its name (= the id) or by its
 * command's program (= Binary). nullptr if none. */
const Integration *match_integration(const std::vector<Integration> &all,
		std::string_view name, std::string_view command);

/* The programs in `names` that a shell script starts (a word of a line
 * that isn't a comment): to warn that one in the startup list is also in
 * the autostart script and would run twice. */
/* What wbconf says about a program in its list. The switch is only
 * Waybox's own startup list; the user's autostart script may start the
 * program as well, and either way that should be said. */
struct ProgramStatus {
	bool not_installed = false;
	bool running = false;
	bool twice = false;        /* switched on and in the script too */
	bool from_script = false;  /* switched off, started by the script */
};
ProgramStatus program_status(bool installed, bool running, bool in_script,
		bool switched_on);

std::vector<std::string> programs_in_script(std::string_view script,
		const std::vector<std::string> &names);

}  // namespace wb

#endif
