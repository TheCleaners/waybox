#ifndef WB_GTK_APPEARANCE_HPP
#define WB_GTK_APPEARANCE_HPP

/*
 * The themes GTK programs use (icons, cursors, GTK themes), as wbconf
 * shows them: what's installed, and icon themes whose fallback chain is
 * broken (an Inherits= theme that isn't installed: GTK then shows its
 * missing-icon placeholder). Files only, no GLib.
 */
#include <map>
#include <string>
#include <vector>

namespace wb {

/* Where icon themes are looked up, in GTK's order: ~/.icons,
 * $XDG_DATA_HOME/icons, each $XDG_DATA_DIRS/icons, /usr/share/pixmaps. */
std::vector<std::string> icon_theme_dirs(const std::string &home,
		const std::string &xdg_data_home, const std::string &xdg_data_dirs);
/* Where GTK themes are: ~/.themes, $XDG_DATA_HOME/themes, each
 * $XDG_DATA_DIRS/themes. */
std::vector<std::string> gtk_theme_dirs(const std::string &home,
		const std::string &xdg_data_home, const std::string &xdg_data_dirs);

struct IconTheme {
	std::string id;                     /* its directory's name */
	std::string name;                   /* Name= */
	std::string path;                   /* its directory (the first found) */
	std::vector<std::string> inherits;
	bool has_icons = false;             /* Directories= isn't empty */
	bool has_cursors = false;           /* a cursors/ subdirectory */
	bool hidden = false;                /* Hidden=true */
	bool stand_in = false;              /* made by wbconf (see below) */
	/* A stand-in, while a real theme of that name is installed in a later
	 * directory: the stand-in hides it, and has to go. */
	bool hides_real = false;
};

/* Installed icon themes by id (the first directory in `dirs` wins). */
std::map<std::string, IconTheme> find_icon_themes(const std::vector<std::string> &dirs);

/* GTK themes that have a gtk-3.0 or gtk-4.0 folder, sorted. */
std::vector<std::string> find_gtk_themes(const std::vector<std::string> &dirs);

/* The themes `id`'s fallback chain names that aren't installed, in the
 * order GTK would look ("hicolor" always counts as part of the chain). */
std::vector<std::string> missing_fallbacks(const std::string &id,
		const std::map<std::string, IconTheme> &themes);

/* A complete theme to fill gaps from: Adwaita, else breeze, else any with
 * icons and no gaps of its own ("" if none). */
std::string complete_icon_theme(const std::map<std::string, IconTheme> &themes);

/* The index.theme of a stand-in for the missing theme `missing`, that
 * falls back to `target`: hidden from theme lists, and marked as made by
 * wbconf (X-Waybox-Stand-In=true) so it can be removed again. */
std::string stand_in_theme(const std::string &missing, const std::string &target);

}  // namespace wb

#endif
