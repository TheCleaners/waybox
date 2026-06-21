#ifndef WB_ICON_HPP
#define WB_ICON_HPP

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

/* Convenience: resolve against the real environment is done by the caller; this
 * stays pure. */

}  // namespace wb

#endif /* WB_ICON_HPP */
