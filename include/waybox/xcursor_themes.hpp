#ifndef WB_XCURSOR_THEMES_HPP
#define WB_XCURSOR_THEMES_HPP

#include <string>
#include <vector>

namespace wb {

/*
 * Cursor themes as wlroots (libXcursor's rules) finds them: whether a
 * theme gives any cursors at all, following Inherits=. A theme without
 * any (an empty "default", say, hiding the system's) leaves wlroots on its
 * built-in cursor, which has one size: the pointer size then does nothing.
 */

/* The directories searched, in order: XCURSOR_PATH if set (colon-
 * separated); else $XDG_DATA_HOME/icons (~/.local/share/icons), ~/.icons,
 * /usr/share/icons, /usr/share/pixmaps, ~/.cursors,
 * /usr/share/cursors/xorg-x11, /usr/local/share/icons. "~" is `home`. */
std::vector<std::string> xcursor_search_path(const char *xcursor_path, const char *xdg_data_home,
		const std::string &home);

struct XcursorThemeCheck {
	bool has_cursors = false;
	/* The index.theme that decided the theme's fallback (the first found,
	 * as wlroots reads only that one); empty if none. */
	std::string index_file;
	/* Its Inherits=, as read; empty when it has none or it's empty. */
	std::vector<std::string> inherits;
};

/* `theme` empty means "default", as for wlroots. */
XcursorThemeCheck xcursor_theme_check(const std::string &theme,
		const std::vector<std::string> &path);

/* The themes in `path` with cursors of their own, sorted, no duplicates. */
std::vector<std::string> xcursor_themes_with_cursors(const std::vector<std::string> &path);

/* What to use instead of a theme without cursors: Adwaita if it has
 * any, else the first theme that has (by name); empty if none. */
std::string xcursor_fallback_theme(const std::vector<std::string> &path);

}  // namespace wb

#endif
