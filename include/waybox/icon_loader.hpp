#ifndef WB_ICON_LOADER_HPP
#define WB_ICON_LOADER_HPP

#include <functional>
#include <memory>
#include <vector>
#include <map>

#include "waybox/threads.hpp"
#include <string>
#include <string_view>

#include <cairo.h>

namespace wb {

/*
 * Loading icons into Cairo surfaces: themed icons (menus, window icons) and
 * the icon an application's desktop file names. The file lookup itself is
 * the pure icon.hpp.
 */

/* The user's icon theme: $WB_ICON_THEME, else GTK's gtk-icon-theme-name
 * (gtk-4.0/gtk-3.0 settings.ini), else empty (hicolor). */
std::string detect_icon_theme();

/* Icon `name` from `theme` (then hicolor and pixmaps) at about `size` px, as
 * a new surface the caller owns; nullptr when there is none. PNG, and SVG
 * when built with librsvg. An absolute path is loaded as is. */
cairo_surface_t *load_themed_icon(const std::string &name, const std::string &theme,
		int size);
/* Where icon themes are looked for ($XDG_DATA_HOME/icons, ~/.icons, each
 * $XDG_DATA_DIRS/icons, pixmaps), read from the environment: on the main
 * thread only (Waybox sets environment variables there). */
std::vector<std::string> icon_search_dirs();
/* As above with the folders given: safe on any thread. */
cairo_surface_t *load_themed_icon(const std::string &name, const std::string &theme,
		int size, const std::vector<std::string> &dirs);

/* Icons loaded once and kept, for whatever draws them again (each menu
 * opening): by name, theme and size; a missing icon is remembered too, so
 * it's never looked for again. get() returns a new reference the caller
 * releases (cairo_surface_destroy), or nullptr; clear() (a reconfigure:
 * maybe another theme) drops the cache's own, so a surface still in use
 * stays valid until its user lets go. */
class IconCache {
public:
	IconCache() = default;
	~IconCache();
	IconCache(const IconCache &) = delete;
	IconCache &operator=(const IconCache &) = delete;

	cairo_surface_t *get(const std::string &name, const std::string &theme, int size);
	void clear();
	size_t size() const { return icons_.size(); }
	/* Loads `names` not cached yet on `workers`, in the background; each
	 * comes back through `queue` and is kept, unless the cache was cleared
	 * meanwhile. On the main thread; `done` (main thread) once all are back,
	 * with how many it loaded. */
	void preload(const std::vector<std::string> &names, const std::string &theme, int size,
			Workers &workers, const std::shared_ptr<MainQueue> &queue,
			std::function<void(size_t)> done = {});

private:
	static std::string key(const std::string &name, const std::string &theme, int size);
	std::map<std::string, cairo_surface_t *> icons_;
	/* Bumped by clear(): a preload's results from before are dropped. */
	std::shared_ptr<uint64_t> generation_ = std::make_shared<uint64_t>(0);
};

/* The Icon= of the desktop file for application `app_id` (APP_ID.desktop,
 * also lowercased, in $XDG_DATA_HOME and $XDG_DATA_DIRS applications/), or
 * "". */
std::string desktop_file_icon(std::string_view app_id);

}  // namespace wb

#endif /* WB_ICON_LOADER_HPP */
