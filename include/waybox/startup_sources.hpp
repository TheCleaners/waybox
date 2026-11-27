#ifndef WB_STARTUP_SOURCES_HPP
#define WB_STARTUP_SOURCES_HPP

/*
 * The places programs start from besides Waybox's own startup list, as
 * wbconf shows them: the user's autostart script and XDG autostart.
 * Pure parsing and editing (no files, no GUI).
 */
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/* A program the autostart script starts on a line of its own, outside any
 * if/case/loop/function: "prog args &", or guarded as
 * "command -v prog >/dev/null 2>&1 && prog args &" (or with which). Such a
 * line can be switched off and on by commenting it out; a commented one
 * of that shape counts too (switched off). */
struct ScriptEntry {
	std::string program;  /* basename of what runs */
	std::string command;  /* the line without its comment mark */
	int line = 0;         /* 0-based */
	bool enabled = true;
	bool function = false;  /* runs a function the script defines */
	/* "(waybar || yambar) &": the ones run if the first can't be. */
	std::vector<std::string> alternatives;
};
std::vector<ScriptEntry> script_entries(std::string_view script);

/* Where an entry read earlier (its command, on line `was`) is now, the
 * script having changed since: the entry with the same command nearest
 * that line; -1 if it's gone. */
int find_script_entry(const std::vector<ScriptEntry> &now, const std::string &command, int was);

/* The script with that entry's line commented out or back in (unchanged
 * if the line isn't such an entry). The rest stays byte for byte. */
std::string script_set_enabled(std::string_view script, int line, bool on);

/* An XDG autostart entry (Desktop Application Autostart Specification), as
 * Waybox's xdg-autostart treats it. */
struct AutostartEntry {
	std::string file;      /* e.g. "foo.desktop": the id, for shadowing */
	std::string path;      /* the file that counts (the user's shadows) */
	bool user = false;     /* that file is in the user's directory */
	std::string name;
	std::string exec;      /* without field codes */
	std::string program;   /* basename of what runs */
	bool enabled = true;   /* not Hidden, not X-GNOME-Autostart-enabled=false */
	bool for_here = true;  /* OnlyShowIn/NotShowIn allow these desktops */
};

/* How the autostart script starts `program`: the lines that can be
 * switched off to stop it (its own start lines, and lines calling a
 * function of the script that runs it), and whether it's also started
 * some way no line switches (an if block, a pipeline, a function no
 * line calls). Lines already commented out don't count. */
struct ScriptStarters {
	std::vector<ScriptEntry> lines;
	std::vector<std::string> functions;  /* of those lines: the functions */
	bool elsewhere = false;
};
ScriptStarters script_starters(std::string_view script, const std::string &program);

/* The command a start line runs, for <startup>: without the final "&" and
 * a "command -v prog >/dev/null 2>&1 &&" guard ("swaybg -c '#000' &" ->
 * "swaybg -c '#000'"). */
std::string script_start_command(std::string_view line);

/* Parse one .desktop file's text into `e` (its file/path/user unset);
 * false if it has no [Desktop Entry] group or no Exec. */
bool parse_autostart(std::string_view text, const std::vector<std::string> &desktops,
		AutostartEntry &e);

/* The .desktop text switched on or off for `desktop` only (Waybox's
 * $XDG_CURRENT_DESKTOP): off adds it to NotShowIn (out of OnlyShowIn); on
 * takes it out of NotShowIn, into an OnlyShowIn that leaves it out, and
 * drops Hidden and X-GNOME-Autostart-enabled=false. Other desktops keep
 * starting it as before (~/.config/autostart is shared by them all). Only
 * the [Desktop Entry] group is touched. */
std::string autostart_set_enabled(std::string_view text, bool on,
		const std::string &desktop = "Waybox");

}  // namespace wb

#endif
