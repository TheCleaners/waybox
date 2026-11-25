#ifndef WB_APP_MENU_HPP
#define WB_APP_MENU_HPP

#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "waybox/menu.hpp"

namespace wb {

/*
 * The applications menu (menu.xml <menu type="applications">): installed
 * programs from their .desktop files, by XDG main category.
 *
 * Parsing, filtering and building the menu are pure and unit-tested; the
 * scan reads the applications directories ($XDG_DATA_HOME, then
 * $XDG_DATA_DIRS, each /applications), and ApplicationsMenu keeps the
 * result until one of them changes.
 */

struct DesktopEntry {
	std::string id;     /* e.g. org.gnome.Nautilus.desktop (subdirs: a-b.desktop) */
	std::string name;   /* localized when there's a Name[...] for the locale */
	std::string exec;   /* unescaped, without field codes */
	std::string icon;
	std::string path;   /* working directory (Path=) */
	std::string try_exec;
	std::vector<std::string> categories, only_show_in, not_show_in;
	bool application = false;  /* Type=Application */
	bool terminal = false, no_display = false, hidden = false;
};

/* The locale names to look Name[...] up by, best first, from a POSIX
 * locale ("de_DE.UTF-8@euro" -> de_DE@euro, de_DE, de@euro, de). */
std::vector<std::string> locale_names(std::string_view locale);

/* One .desktop file's [Desktop Entry]; false if there is none. */
bool parse_desktop_entry(std::string_view text, const std::vector<std::string> &langs,
		DesktopEntry &e);

/* Whether it belongs in the menu: an application with a command, not
 * NoDisplay or Hidden, allowed on these desktops (OnlyShowIn/NotShowIn
 * against $XDG_CURRENT_DESKTOP's list). TryExec is the scan's to check. */
bool desktop_entry_shown(const DesktopEntry &e, const std::vector<std::string> &desktops);

/* Its main category's key (AudioVideo, Development, ... or "Other"). */
std::string main_category(const DesktopEntry &e);

/* The shell command that starts it: Terminal=true ones in `terminal`
 * ("<terminal> -e <command>"), in Path= if it has one. */
std::string desktop_entry_command(const DesktopEntry &e, std::string_view terminal);

/* The menu `id` (label `label`): a submenu per category that has
 * programs (`id`/<category>), each sorted by name. */
MenuFile build_applications_menu(const std::string &id, const std::string &label,
		const std::vector<DesktopEntry> &entries, std::string_view terminal);

/* ---- reading them from disk ------------------------------------------- */

/* $XDG_DATA_HOME/applications, then each of $XDG_DATA_DIRS' (with the
 * spec's defaults when unset). */
std::vector<std::string> application_dirs(const char *data_home, const char *home,
		const char *data_dirs);

/* The entries shown on these desktops; an id found first (the user's
 * directory) shadows the same id later, Hidden or not. */
std::vector<DesktopEntry> scan_applications(const std::vector<std::string> &dirs,
		const std::vector<std::string> &langs, const std::vector<std::string> &desktops);

/* The menu, built again only when an applications directory (or one in
 * it) changed since. */
class ApplicationsMenu {
public:
	const MenuFile &get(const std::string &id, const std::string &label,
			const std::vector<std::string> &dirs, const std::vector<std::string> &langs,
			const std::vector<std::string> &desktops, const std::string &terminal);
	void invalidate() { stamps_.clear(); }

private:
	using Stamp = std::pair<std::string, struct timespec>;
	std::vector<Stamp> stamps_;  /* every directory read, with its mtime */
	std::string key_;            /* what it was built for */
	MenuFile menu_;
};

}  // namespace wb

#endif
