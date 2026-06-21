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

/* A font, by Pango family/size. Lives here (not render.hpp) so the pure theme
 * and style layers can reference it without pulling in Cairo. */
struct FontSpec {
	const char *family = "sans";
	int size_pt = 10;
	bool bold = false;
};

/* Text alignment for labels (themerc *.text.justify). */
enum class Justify {
	Left,
	Center,
	Right,
};

/*
 * Raw themerc sections. These mirror the file's key namespaces 1:1 (colours and
 * textures as written); the resolved, render-ready presentation structs live in
 * style.hpp and are derived from these. Keeping the file model separate from the
 * render model lets richer themes/settings compose presentation from more than
 * just themerc later.
 */

/* themerc window.{active,inactive}.* — one full per-state decoration section. */
struct WindowColors {
	Texture title_bg;          /* window.*.title.bg */
	Texture label_bg;          /* window.*.label.bg */
	Color label_text;          /* window.*.label.text.color */
	Color border_color{0, 0, 0, 255};  /* window.*.border.color */
	Color client_color{0, 0, 0, 255};  /* window.*.client.color (frame/client line) */
	Texture handle_bg;         /* window.*.handle.bg */
	Texture grip_bg;           /* window.*.grip.bg */
	Texture button_bg;         /* window.*.button.unpressed.bg */
	Texture button_hover_bg;   /* window.*.button.*.hover.bg (accent on hover) */
	Texture button_pressed_bg; /* window.*.button.*.pressed.bg (held-down) */
	bool button_bg_parentrelative = false;  /* normal button bg = titlebar bg */
	Color button_icon{0, 0, 0, 255};          /* button.unpressed.image.color */
	Color button_icon_hover{0, 0, 0, 255};    /* button.hover.image.color */
	Color button_icon_pressed{0, 0, 0, 255};  /* button.pressed.image.color */
	Color button_icon_disabled{0x80, 0x80, 0x80, 255};  /* button.disabled.image.color */
};

/* themerc menu.* */
struct MenuColors {
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
	Color border_color{0, 0, 0, 255};  /* global fallback border colour */
	int padding_x = 4;
	int padding_y = 3;
	int handle_width = 0;              /* window.handle.width */
	Justify label_justify = Justify::Left;  /* window.label.text.justify */
	int menu_overlap_x = 0;
	int menu_overlap_y = 0;

	/* waybox extensions (Openbox ignores these themerc keys). */
	int menu_corner_radius = 0;        /* waybox.menu.corner.radius */
	int menu_item_spacing = 0;         /* waybox.menu.item.spacing */
	double menu_opacity = 1.0;         /* waybox.menu.opacity (0..1) */
	int window_corner_radius = 0;      /* waybox.window.corner.radius */

	WindowColors window_active;
	WindowColors window_inactive;
	MenuColors menu;
};

/*
 * Parse a colour as written in themerc: "#rgb", "#rrggbb", "#rrggbbaa",
 * X11 "rgb:rr/gg/bb", or a small set of named colours (case-insensitive).
 * Returns std::nullopt if it cannot be parsed.
 */
std::optional<Color> parse_color(std::string_view spec);

/* Parse a themerc justify value ("left"/"center"/"centre"/"right"). */
std::optional<Justify> justify_from_name(std::string_view name);

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
