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
 *   Kind=binding          (run from a key, not at startup)
 *   Key=W-period          (the key it suggests)
 *
 *   [Option latitude]
 *   Label=Latitude
 *   Flag=-l
 */
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

struct IntegrationOption {
	std::string id;      /* from [Option id] */
	std::string label;
	std::string flag;    /* "-l": the value is the next word */
	std::string hint;    /* example value, shown when empty */
	bool operator==(const IntegrationOption &) const = default;
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
	bool operator==(const Integration &) const = default;
};

/* The version in a program's version output ("swaybg version 1.2.2",
 * "Waybar v0.15.0", "ibus-daemon - Version 1.5.35"): the first word that
 * is a version number, without a leading "v"; "" if none. */
std::string version_from_output(std::string_view output);

/* Parse one file's text; nullopt without an [Integration] Name and Exec. */
std::optional<Integration> parse_integration(std::string_view id, std::string_view text);

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
 * the flag, a new one is appended; everything else is kept as it was. */
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
