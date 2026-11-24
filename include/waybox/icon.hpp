#ifndef WB_ICON_HPP
#define WB_ICON_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * Icon lookup (pure path resolution, XDG Icon Theme Specification).
 *
 * Given an icon name (e.g. "firefox") plus a theme, a desired size and the
 * relevant XDG base dirs, produce the ordered list of candidate file paths to
 * try, most-preferred first. The caller stats them in order and loads the first
 * that exists (done outside this pure, unit-tested code).
 *
 * The algorithm is a pragmatic subset of the full spec: search the named theme
 * then the "hicolor" fallback, preferring an exact size match, then larger,
 * then "scalable", across the common icon contexts; finally fall back to the
 * legacy pixmaps dir. An absolute path (or a name with a recognised image
 * extension that exists as given) is returned verbatim.
 */

/* Candidate icon-theme base directories, most-preferred first:
 * $XDG_DATA_HOME/icons (or ~/.local/share/icons), ~/.icons, then each
 * $XDG_DATA_DIRS/icons (default /usr/local/share, /usr/share), plus
 * /usr/share/pixmaps. Pure (env passed in). */
std::vector<std::string> icon_base_dirs(std::string_view home,
		std::string_view xdg_data_home, std::string_view xdg_data_dirs);

/*
 * Ordered candidate file paths for an icon named `name` at `size` (px) in
 * `theme`. If `name` is an absolute path it is returned as the sole candidate.
 * Otherwise paths of the form <base>/<theme>/<size>/<context>/<name>.<ext> are
 * generated (then larger sizes, then scalable, then the hicolor fallback theme,
 * then <base>/pixmaps/<name>.<ext>). Extensions tried: png, svg, xpm.
 */
std::vector<std::string> icon_search_paths(std::string_view name,
		std::string_view theme, int size,
		const std::vector<std::string> &base_dirs);

/* One entry of an icon theme's index.theme `Directories=` list. */
struct IconThemeDir {
	enum class Type { Fixed, Scalable, Threshold };
	std::string path;   /* e.g. "48x48/apps" or "categories/24" */
	int size = 0;
	int min_size = 0;
	int max_size = 0;
	int threshold = 2;
	int scale = 1;
	Type type = Type::Threshold;
};

/* A parsed index.theme: its directories and the themes it inherits from. */
struct IconThemeIndex {
	std::vector<std::string> inherits;
	std::vector<IconThemeDir> dirs;
};

/* Parse an index.theme file (Icon Theme Specification). Unknown keys are
 * ignored; directories listed but without a section get the spec defaults. */
IconThemeIndex parse_icon_theme_index(std::string_view contents);

/* The theme's directories for an icon of `size` px, best first: directories
 * that match the size, then by the spec's size distance (larger icons first
 * on ties, since downscaling looks better). Scale>1 (HiDPI) directories come
 * after all scale-1 ones. */
std::vector<std::string> icon_theme_dirs_for_size(const IconThemeIndex &index,
		int size);

/* Returns a theme's parsed index, or null if the theme has none (it is then
 * searched with the generic <size>x<size>/<context> layout). */
using IconThemeIndexLookup =
		std::function<const IconThemeIndex *(std::string_view theme)>;

/*
 * Spec-following variant: searches `theme`, then the themes it inherits
 * (depth first, each once), then hicolor, each in its own directory order
 * from index.theme (so themes with any layout work, e.g. Breeze's
 * <context>/<size>), then <base>/pixmaps.
 */
std::vector<std::string> icon_search_paths(std::string_view name,
		std::string_view theme, int size,
		const std::vector<std::string> &base_dirs,
		const IconThemeIndexLookup &index_of);

/* Convenience: resolve against the real environment is done by the caller; this
 * stays pure. */

/* The Icon= of a desktop file's [Desktop Entry] group, or "". */
std::string desktop_entry_icon(std::string_view contents);

}  // namespace wb

#endif /* WB_ICON_HPP */
