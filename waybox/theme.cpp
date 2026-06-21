/*
 * Theme model + themerc parsing/loading. Depends only on the C++ standard
 * library (no wlroots), so it links into the standalone unit test and the
 * tests_only sanitized CI build.
 */
#include "waybox/theme.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace wb {

namespace {

std::string to_lower(std::string_view s) {
	std::string out(s);
	std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

std::string_view trim(std::string_view s) {
	auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
	while (!s.empty() && is_space(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && is_space(s.back()))
		s.remove_suffix(1);
	return s;
}

std::optional<int> hex_nibble(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	return std::nullopt;
}

std::optional<int> hex_byte(char hi, char lo) {
	auto h = hex_nibble(hi);
	auto l = hex_nibble(lo);
	if (!h || !l)
		return std::nullopt;
	return (*h << 4) | *l;
}

struct NamedColor {
	std::string_view name;
	Color color;
};

constexpr std::array<NamedColor, 12> kNamedColors = {{
	{"black", {0, 0, 0, 255}},
	{"white", {255, 255, 255, 255}},
	{"gray", {190, 190, 190, 255}},
	{"grey", {190, 190, 190, 255}},
	{"darkgray", {64, 64, 64, 255}},
	{"darkgrey", {64, 64, 64, 255}},
	{"lightgray", {211, 211, 211, 255}},
	{"lightgrey", {211, 211, 211, 255}},
	{"red", {255, 0, 0, 255}},
	{"green", {0, 128, 0, 255}},
	{"blue", {0, 0, 255, 255}},
	{"yellow", {255, 255, 0, 255}},
}};

}  // namespace

std::optional<Color> parse_color(std::string_view spec) {
	spec = trim(spec);
	if (spec.empty())
		return std::nullopt;

	if (spec.front() == '#') {
		std::string_view hex = spec.substr(1);
		Color c;
		if (hex.size() == 3) {  /* #rgb -> #rrggbb */
			auto r = hex_byte(hex[0], hex[0]);
			auto g = hex_byte(hex[1], hex[1]);
			auto b = hex_byte(hex[2], hex[2]);
			if (!r || !g || !b)
				return std::nullopt;
			c.r = *r; c.g = *g; c.b = *b;
			return c;
		}
		if (hex.size() == 6 || hex.size() == 8) {
			auto r = hex_byte(hex[0], hex[1]);
			auto g = hex_byte(hex[2], hex[3]);
			auto b = hex_byte(hex[4], hex[5]);
			if (!r || !g || !b)
				return std::nullopt;
			c.r = *r; c.g = *g; c.b = *b;
			if (hex.size() == 8) {
				auto a = hex_byte(hex[6], hex[7]);
				if (!a)
					return std::nullopt;
				c.a = *a;
			}
			return c;
		}
		return std::nullopt;
	}

	/* X11 "rgb:rr/gg/bb" */
	if (spec.size() >= 4 && to_lower(spec.substr(0, 4)) == "rgb:") {
		std::string rest(spec.substr(4));
		std::array<int, 3> vals{};
		size_t idx = 0;
		std::stringstream ss(rest);
		std::string part;
		while (std::getline(ss, part, '/') && idx < 3) {
			if (part.size() < 2)
				return std::nullopt;
			auto byte = hex_byte(part[0], part[1]);
			if (!byte)
				return std::nullopt;
			vals[idx++] = *byte;
		}
		if (idx != 3)
			return std::nullopt;
		return Color{static_cast<uint8_t>(vals[0]), static_cast<uint8_t>(vals[1]),
				static_cast<uint8_t>(vals[2]), 255};
	}

	std::string lower = to_lower(spec);
	for (const NamedColor &named : kNamedColors) {
		if (lower == named.name)
			return named.color;
	}
	return std::nullopt;
}

Theme default_theme() {
	Theme t;
	t.window_active.title_bg.color = {0x30, 0x50, 0x80, 0xff};
	t.window_active.label_text = {0xff, 0xff, 0xff, 0xff};
	t.window_active.border_color = {0x10, 0x20, 0x40, 0xff};
	t.window_active.button_icon = {0xff, 0xff, 0xff, 0xff};
	t.window_active.button_icon_hover = {0xff, 0xff, 0xff, 0xff};
	t.window_active.button_icon_pressed = {0xc0, 0xc0, 0xc0, 0xff};
	t.window_active.button_bg_parentrelative = true;
	t.window_active.button_hover_bg.color = {0x50, 0x70, 0xa0, 0xff};
	t.window_inactive.title_bg.color = {0x80, 0x80, 0x80, 0xff};
	t.window_inactive.label_text = {0xd0, 0xd0, 0xd0, 0xff};
	t.window_inactive.border_color = {0x50, 0x50, 0x50, 0xff};
	t.window_inactive.button_icon = {0xd0, 0xd0, 0xd0, 0xff};
	t.window_inactive.button_icon_hover = {0xff, 0xff, 0xff, 0xff};
	t.window_inactive.button_icon_pressed = {0xa0, 0xa0, 0xa0, 0xff};
	t.window_inactive.button_bg_parentrelative = true;
	t.window_inactive.button_hover_bg.color = {0xa0, 0xa0, 0xa0, 0xff};
	t.menu.title_bg.color = {0x30, 0x50, 0x80, 0xff};
	t.menu.title_text = {0xff, 0xff, 0xff, 0xff};
	t.menu.items_bg.color = {0xe0, 0xe0, 0xe0, 0xff};
	t.menu.items_text = {0x00, 0x00, 0x00, 0xff};
	t.menu.items_active_bg.color = {0x30, 0x50, 0x80, 0xff};
	t.menu.items_active_text = {0xff, 0xff, 0xff, 0xff};
	t.menu.separator = {0x80, 0x80, 0x80, 0xff};
	return t;
}

std::optional<Justify> justify_from_name(std::string_view name) {
	if (name == "left")
		return Justify::Left;
	if (name == "center" || name == "centre")
		return Justify::Center;
	if (name == "right")
		return Justify::Right;
	return std::nullopt;
}

std::optional<FontPlace> font_place_from_name(std::string_view name) {
	std::string n;
	n.reserve(name.size());
	for (char c : name)
		n.push_back(static_cast<char>(std::tolower(
				static_cast<unsigned char>(c))));
	if (n == "activewindow")
		return FontPlace::ActiveWindow;
	if (n == "inactivewindow")
		return FontPlace::InactiveWindow;
	if (n == "menuheader")
		return FontPlace::MenuHeader;
	if (n == "menuitem")
		return FontPlace::MenuItem;
	/* Openbox distinguishes active/inactive OSD; waybox uses one OSD font. */
	if (n == "activeonscreendisplay" || n == "inactiveonscreendisplay" ||
			n == "onscreendisplay")
		return FontPlace::OnScreenDisplay;
	return std::nullopt;
}

namespace {

void apply_color(std::string_view value, Color &out) {
	if (auto c = parse_color(value))
		out = *c;
}

void apply_texture_type(std::string_view value, Texture &out) {
	out.type = to_lower(value).find("gradient") != std::string::npos
			? TextureType::Gradient
			: TextureType::Solid;
}

std::optional<int> parse_int(std::string_view value) {
	value = trim(value);
	if (value.empty())
		return std::nullopt;
	std::string s(value);  /* keep the buffer alive for end-pointer below */
	char *end = nullptr;
	long v = std::strtol(s.c_str(), &end, 10);
	if (end == nullptr || *end != '\0')
		return std::nullopt;
	return static_cast<int>(v);
}

std::optional<double> parse_double(std::string_view value) {
	value = trim(value);
	if (value.empty())
		return std::nullopt;
	std::string s(value);
	char *end = nullptr;
	double v = std::strtod(s.c_str(), &end);
	if (end == nullptr || *end != '\0')
		return std::nullopt;
	return v;
}

/* Pointer to the Color a color-valued key targets, or nullptr. Centralising
 * this lets the ".alpha" extension key reuse the same routing. */
Color *color_field_for(Theme &t, std::string_view key) {
	if (key == "border.color") return &t.border_color;
	if (key == "window.active.title.bg.color") return &t.window_active.title_bg.color;
	if (key == "window.active.title.bg.colorTo") return &t.window_active.title_bg.color_to;
	if (key == "window.active.label.bg.color") return &t.window_active.label_bg.color;
	if (key == "window.active.label.bg.colorTo") return &t.window_active.label_bg.color_to;
	if (key == "window.active.label.text.color") return &t.window_active.label_text;
	if (key == "window.active.border.color") return &t.window_active.border_color;
	if (key == "window.active.client.color") return &t.window_active.client_color;
	if (key == "window.active.handle.bg.color") return &t.window_active.handle_bg.color;
	if (key == "window.active.handle.bg.colorTo") return &t.window_active.handle_bg.color_to;
	if (key == "window.active.grip.bg.color") return &t.window_active.grip_bg.color;
	if (key == "window.active.grip.bg.colorTo") return &t.window_active.grip_bg.color_to;
	if (key == "window.active.button.unpressed.bg.color") return &t.window_active.button_bg.color;
	if (key == "window.active.button.unpressed.image.color") return &t.window_active.button_icon;
	if (key == "window.active.button.hover.image.color") return &t.window_active.button_icon_hover;
	if (key == "window.active.button.pressed.image.color") return &t.window_active.button_icon_pressed;
	if (key == "window.active.button.disabled.image.color") return &t.window_active.button_icon_disabled;
	/* Openbox wildcard button keys (apply to all buttons), used by themes like
	 * Onyx-Citrus that don't name each button individually. */
	if (key == "window.active.button.*.image.color") return &t.window_active.button_icon;
	if (key == "window.active.button.*.hover.image.color") return &t.window_active.button_icon_hover;
	if (key == "window.active.button.*.pressed.image.color") return &t.window_active.button_icon_pressed;
	if (key == "window.active.button.*.disabled.image.color") return &t.window_active.button_icon_disabled;
	if (key == "window.active.button.*.hover.bg.color") return &t.window_active.button_hover_bg.color;
	if (key == "window.active.button.*.hover.bg.colorTo") return &t.window_active.button_hover_bg.color_to;
	if (key == "window.active.button.*.pressed.bg.color") return &t.window_active.button_pressed_bg.color;
	if (key == "window.active.button.*.pressed.bg.colorTo") return &t.window_active.button_pressed_bg.color_to;
	if (key == "window.inactive.title.bg.color") return &t.window_inactive.title_bg.color;
	if (key == "window.inactive.title.bg.colorTo") return &t.window_inactive.title_bg.color_to;
	if (key == "window.inactive.label.bg.color") return &t.window_inactive.label_bg.color;
	if (key == "window.inactive.label.bg.colorTo") return &t.window_inactive.label_bg.color_to;
	if (key == "window.inactive.label.text.color") return &t.window_inactive.label_text;
	if (key == "window.inactive.border.color") return &t.window_inactive.border_color;
	if (key == "window.inactive.client.color") return &t.window_inactive.client_color;
	if (key == "window.inactive.handle.bg.color") return &t.window_inactive.handle_bg.color;
	if (key == "window.inactive.handle.bg.colorTo") return &t.window_inactive.handle_bg.color_to;
	if (key == "window.inactive.grip.bg.color") return &t.window_inactive.grip_bg.color;
	if (key == "window.inactive.grip.bg.colorTo") return &t.window_inactive.grip_bg.color_to;
	if (key == "window.inactive.button.unpressed.bg.color") return &t.window_inactive.button_bg.color;
	if (key == "window.inactive.button.unpressed.image.color") return &t.window_inactive.button_icon;
	if (key == "window.inactive.button.hover.image.color") return &t.window_inactive.button_icon_hover;
	if (key == "window.inactive.button.pressed.image.color") return &t.window_inactive.button_icon_pressed;
	if (key == "window.inactive.button.disabled.image.color") return &t.window_inactive.button_icon_disabled;
	if (key == "window.inactive.button.*.image.color") return &t.window_inactive.button_icon;
	if (key == "window.inactive.button.*.hover.image.color") return &t.window_inactive.button_icon_hover;
	if (key == "window.inactive.button.*.pressed.image.color") return &t.window_inactive.button_icon_pressed;
	if (key == "window.inactive.button.*.disabled.image.color") return &t.window_inactive.button_icon_disabled;
	if (key == "window.inactive.button.*.hover.bg.color") return &t.window_inactive.button_hover_bg.color;
	if (key == "window.inactive.button.*.hover.bg.colorTo") return &t.window_inactive.button_hover_bg.color_to;
	if (key == "window.inactive.button.*.pressed.bg.color") return &t.window_inactive.button_pressed_bg.color;
	if (key == "window.inactive.button.*.pressed.bg.colorTo") return &t.window_inactive.button_pressed_bg.color_to;
	if (key == "menu.title.bg.color") return &t.menu.title_bg.color;
	if (key == "menu.title.bg.colorTo") return &t.menu.title_bg.color_to;
	if (key == "menu.title.text.color") return &t.menu.title_text;
	if (key == "menu.items.bg.color") return &t.menu.items_bg.color;
	if (key == "menu.items.bg.colorTo") return &t.menu.items_bg.color_to;
	if (key == "menu.items.text.color") return &t.menu.items_text;
	if (key == "menu.items.active.bg.color") return &t.menu.items_active_bg.color;
	if (key == "menu.items.active.bg.colorTo") return &t.menu.items_active_bg.color_to;
	if (key == "menu.items.active.text.color") return &t.menu.items_active_text;
	if (key == "menu.separator.color") return &t.menu.separator;
	return nullptr;
}

bool ends_with(std::string_view s, std::string_view suffix) {
	return s.size() >= suffix.size() &&
			s.substr(s.size() - suffix.size()) == suffix;
}

void apply_key(Theme &t, std::string_view key, std::string_view value) {
	auto set_int = [&](int &dst) {
		if (auto v = parse_int(value))
			dst = *v;
	};

	if (key == "border.width") { set_int(t.border_width); return; }
	if (key == "padding.width") { set_int(t.padding_x); return; }
	if (key == "padding.height") { set_int(t.padding_y); return; }
	if (key == "window.handle.width") { set_int(t.handle_width); return; }
	if (key == "menu.overlap.x" || key == "menu.overlap") { set_int(t.menu_overlap_x); return; }
	if (key == "menu.overlap.y") { set_int(t.menu_overlap_y); return; }

	/* Label justification (themerc window.label.text.justify). */
	if (key == "window.label.text.justify" ||
			key == "window.active.label.text.justify" ||
			key == "window.inactive.label.text.justify") {
		if (auto j = justify_from_name(to_lower(value)))
			t.label_justify = *j;
		return;
	}

	/* waybox themerc extensions (Openbox ignores these keys). */
	if (key == "waybox.menu.corner.radius") { set_int(t.menu_corner_radius); return; }
	if (key == "waybox.menu.item.spacing") { set_int(t.menu_item_spacing); return; }
	if (key == "waybox.window.corner.radius") { set_int(t.window_corner_radius); return; }
	if (key == "waybox.menu.opacity") {
		if (auto v = parse_double(value)) {
			double clamped = *v < 0.0 ? 0.0 : (*v > 1.0 ? 1.0 : *v);
			t.menu_opacity = clamped;
		}
		return;
	}

	/* Texture type lines (the bare ".bg" without ".color"). */
	if (key == "window.active.title.bg") { apply_texture_type(value, t.window_active.title_bg); return; }
	if (key == "window.active.label.bg") { apply_texture_type(value, t.window_active.label_bg); return; }
	if (key == "window.active.handle.bg") { apply_texture_type(value, t.window_active.handle_bg); return; }
	if (key == "window.active.grip.bg") { apply_texture_type(value, t.window_active.grip_bg); return; }
	if (key == "window.active.button.unpressed.bg") { apply_texture_type(value, t.window_active.button_bg); return; }
	if (key == "window.active.button.*.hover.bg") { apply_texture_type(value, t.window_active.button_hover_bg); return; }
	if (key == "window.active.button.*.pressed.bg") { apply_texture_type(value, t.window_active.button_pressed_bg); return; }
	if (key == "window.active.button.*.bg") {
		t.window_active.button_bg_parentrelative =
				to_lower(value).find("parentrelative") != std::string::npos;
		return;
	}
	if (key == "window.*.button.*.bg") {
		bool pr = to_lower(value).find("parentrelative") != std::string::npos;
		t.window_active.button_bg_parentrelative = pr;
		t.window_inactive.button_bg_parentrelative = pr;
		return;
	}
	if (key == "window.inactive.title.bg") { apply_texture_type(value, t.window_inactive.title_bg); return; }
	if (key == "window.inactive.label.bg") { apply_texture_type(value, t.window_inactive.label_bg); return; }
	if (key == "window.inactive.handle.bg") { apply_texture_type(value, t.window_inactive.handle_bg); return; }
	if (key == "window.inactive.grip.bg") { apply_texture_type(value, t.window_inactive.grip_bg); return; }
	if (key == "window.inactive.button.unpressed.bg") { apply_texture_type(value, t.window_inactive.button_bg); return; }
	if (key == "window.inactive.button.*.hover.bg") { apply_texture_type(value, t.window_inactive.button_hover_bg); return; }
	if (key == "window.inactive.button.*.pressed.bg") { apply_texture_type(value, t.window_inactive.button_pressed_bg); return; }
	if (key == "window.inactive.button.*.bg") {
		t.window_inactive.button_bg_parentrelative =
				to_lower(value).find("parentrelative") != std::string::npos;
		return;
	}
	if (key == "menu.title.bg") { apply_texture_type(value, t.menu.title_bg); return; }
	if (key == "menu.items.bg") { apply_texture_type(value, t.menu.items_bg); return; }
	if (key == "menu.items.active.bg") { apply_texture_type(value, t.menu.items_active_bg); return; }

	/* waybox extension: a ".alpha" suffix sets the alpha channel of the named
	 * colour (0-255), so transparency works even for rgb:/named colours that
	 * have no inline alpha. Applied after colours (see parse_themerc) so it is
	 * order-independent. */
	if (ends_with(key, ".alpha")) {
		std::string_view base = key.substr(0, key.size() - 6 /* ".alpha" */);
		if (Color *c = color_field_for(t, base)) {
			if (auto a = parse_int(value)) {
				int clamped = *a < 0 ? 0 : (*a > 255 ? 255 : *a);
				c->a = static_cast<uint8_t>(clamped);
			}
		}
		return;
	}

	if (Color *c = color_field_for(t, key))
		apply_color(value, *c);
}

}  // namespace

Theme parse_themerc(std::string_view contents) {
	Theme theme = default_theme();

	/* Collect the key/value lines, then apply colours before ".alpha" overrides
	 * so the latter are order-independent (a colour assignment resets alpha to
	 * opaque, so it must not run after its own alpha key). */
	std::vector<std::pair<std::string, std::string>> entries;
	size_t pos = 0;
	while (pos <= contents.size()) {
		size_t nl = contents.find('\n', pos);
		std::string_view line = contents.substr(
				pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
		pos = nl == std::string_view::npos ? contents.size() + 1 : nl + 1;

		std::string_view trimmed = trim(line);
		if (trimmed.empty() || trimmed.front() == '!')
			continue;  /* blank or comment */
		size_t colon = trimmed.find(':');
		if (colon == std::string_view::npos)
			continue;
		std::string_view key = trim(trimmed.substr(0, colon));
		std::string_view value = trim(trimmed.substr(colon + 1));
		if (!key.empty())
			entries.emplace_back(std::string(key), std::string(value));
	}

	for (const auto &[key, value] : entries) {
		if (!ends_with(key, ".alpha"))
			apply_key(theme, key, value);
	}
	for (const auto &[key, value] : entries) {
		if (ends_with(key, ".alpha"))
			apply_key(theme, key, value);
	}
	return theme;
}

std::vector<std::string> themerc_search_paths(std::string_view name,
		std::string_view home, std::string_view xdg_data_home,
		std::string_view xdg_data_dirs) {
	namespace fs = std::filesystem;
	std::vector<fs::path> bases;

	if (!xdg_data_home.empty())
		bases.emplace_back(fs::path(xdg_data_home) / "themes");
	else if (!home.empty())
		bases.emplace_back(fs::path(home) / ".local" / "share" / "themes");

	if (!home.empty())
		bases.emplace_back(fs::path(home) / ".themes");

	std::string dirs = xdg_data_dirs.empty() ? "/usr/local/share:/usr/share"
			: std::string(xdg_data_dirs);
	std::stringstream ss(dirs);
	std::string dir;
	while (std::getline(ss, dir, ':')) {
		if (!dir.empty())
			bases.emplace_back(fs::path(dir) / "themes");
	}

	std::vector<std::string> paths;
	paths.reserve(bases.size());
	for (const fs::path &base : bases)
		paths.push_back((base / name / "openbox-3" / "themerc").string());
	return paths;
}

Theme load_theme(std::string_view name) {
	auto env = [](const char *key) -> std::string {
		const char *v = std::getenv(key);
		return v ? v : "";
	};

	std::vector<std::string> candidates = themerc_search_paths(
			name, env("HOME"), env("XDG_DATA_HOME"), env("XDG_DATA_DIRS"));

	std::error_code ec;
	for (const std::string &path : candidates) {
		if (!std::filesystem::exists(path, ec))
			continue;
		std::ifstream file(path, std::ios::binary);
		if (!file)
			continue;
		std::stringstream buffer;
		buffer << file.rdbuf();
		return parse_themerc(buffer.str());
	}
	return default_theme();
}

}  // namespace wb
