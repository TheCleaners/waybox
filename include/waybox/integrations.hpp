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
	std::vector<IntegrationOption> options;
	bool operator==(const Integration &) const = default;
};

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
std::vector<std::string> programs_in_script(std::string_view script,
		const std::vector<std::string> &names);

}  // namespace wb

#endif
