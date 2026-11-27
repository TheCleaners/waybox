#ifndef WB_SETTING_PROBLEMS_HPP
#define WB_SETTING_PROBLEMS_HPP

#include <optional>
#include <string>
#include <vector>

namespace wb {

/*
 * Problems wbconf finds with settings and shows beside them: something
 * set that can't work as meant (a pointer theme without cursors, a theme
 * that isn't installed), why, and what would fix it. Each check is a plain
 * function of what it looks at, so it can be tested on its own.
 */

struct SettingProblem {
	std::string text;       /* what's wrong and why, a sentence or two */
	std::string fix_label;  /* a button doing the fix; empty: none offered */
	bool fixed_on_save = false;  /* a change made but not saved yet fixes it */
};

/* "nwg-look" from an index.theme whose comment says "written by nwg-look"
 * (the programs that write one do); empty if it doesn't say. */
std::string written_by(const std::string &file);

struct PointerState {
	std::string shown;                       /* the theme wbconf shows (GTK's) */
	std::optional<std::string> rc_theme;     /* rc.xml's <cursor theme>, as saved */
	std::string env_theme;                   /* XCURSOR_THEME Waybox was started with */
	bool to_be_written = false;              /* Save will write `shown` to rc.xml */
	std::vector<std::string> path;           /* xcursor_search_path() */
	std::string home;                        /* files under it shown as ~/... */
};

/* `file` with `home` at its start shown as "~" ("~/.local/share/..."). */
std::string tilde(const std::string &file, const std::string &home);

/* Translates a message (wbconf passes gettext; tests, nothing). */
using Translate = const char *(*)(const char *);

/* The pointer Waybox draws: a theme without cursors (Waybox uses another),
 * or another theme than GTK programs use (the pointer changes as it moves
 * between them). nullopt: none. */
std::optional<SettingProblem> pointer_problem(const PointerState &s,
		Translate tr = nullptr);

}  // namespace wb

#endif
