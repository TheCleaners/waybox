#ifndef WB_THEME_HPP
#define WB_THEME_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * Theme model for compositor-drawn UI (menus, titlebars, OSD, SSD).
 *
 * The shape mirrors Openbox's themerc so existing Openbox themes can be parsed
 * verbatim, while still serving richer original themes. This header is the pure
 * data model + parser (no rendering, no wlroots); the scene/Cairo renderer
 * consumes a Theme. Parsing and colour handling are unit-tested.
 */

struct Color {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 255;
};

inline bool operator==(const Color &x, const Color &y) {
	return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
}
inline bool operator!=(const Color &x, const Color &y) { return !(x == y); }

/* A themerc "texture" — a fill for a region. Only the colour(s) and a coarse
 * type are modelled for now; the renderer can start with a solid fill and grow
 * gradient/image support later. */
enum class TextureType {
	Solid,
	Gradient,
};

struct Texture {
	TextureType type = TextureType::Solid;
	Color color;     /* themerc <prefix>.bg.color */
	Color color_to;  /* themerc <prefix>.bg.colorTo (gradient end) */
};

struct WindowStyle {
	Texture title_bg;
	Color label_text;
};

struct MenuStyle {
	Texture title_bg;
	Color title_text;
	Texture items_bg;
	Color items_text;
	Texture items_active_bg;
	Color items_active_text;
	Color separator;
};

struct Theme {
	int border_width = 1;
	Color border_color{0, 0, 0, 255};
	int padding_x = 4;
	int padding_y = 3;
	int menu_overlap_x = 0;
	int menu_overlap_y = 0;
	WindowStyle window_active;
	WindowStyle window_inactive;
	MenuStyle menu;
};

/*
 * Parse a colour as written in themerc: "#rgb", "#rrggbb", "#rrggbbaa",
 * X11 "rgb:rr/gg/bb", or a small set of named colours (case-insensitive).
 * Returns std::nullopt if it cannot be parsed.
 */
std::optional<Color> parse_color(std::string_view spec);

/* A Theme with the built-in defaults (a neutral fallback). */
Theme default_theme();

/*
 * Parse themerc file contents into a Theme, starting from default_theme().
 * Unknown keys and malformed lines are ignored; "!" begins a comment.
 */
Theme parse_themerc(std::string_view contents);

/*
 * Ordered candidate themerc paths for a theme named `name`, following the
 * Openbox/XDG convention <themedir>/<name>/openbox-3/themerc across the theme
 * directories: $XDG_DATA_HOME/themes (or ~/.local/share/themes), ~/.themes,
 * and each $XDG_DATA_DIRS/themes (default /usr/local/share, /usr/share). Pure
 * (env passed in, no filesystem access) so it can be unit-tested; empty env
 * strings fall back to defaults.
 */
std::vector<std::string> themerc_search_paths(std::string_view name,
		std::string_view home, std::string_view xdg_data_home,
		std::string_view xdg_data_dirs);

/*
 * Load the theme named `name`: resolve it via themerc_search_paths() against
 * the real environment, parse the first themerc that exists, or return
 * default_theme() if none is found.
 */
Theme load_theme(std::string_view name);

}  // namespace wb

#endif /* WB_THEME_HPP */
