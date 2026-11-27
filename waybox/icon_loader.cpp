/*
 * Loading icons into Cairo surfaces (see icon_loader.hpp).
 */
#include "waybox/icon_loader.hpp"
#include "waybox/atomic_file.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr off_t kMaxIconFileBytes = 8 * 1024 * 1024;
}

#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

#include "waybox/icon.hpp"

namespace wb {

namespace {

bool ends_with(const std::string &s, const char *suf) {
	size_t n = std::char_traits<char>::length(suf);
	return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

#ifdef HAVE_RSVG
/* Render an SVG file to a square ARGB surface of `size` px via librsvg. */
cairo_surface_t *render_svg(const char *path, int size) {
	GError *err = nullptr;
	RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
	if (h == nullptr) {
		if (err)
			g_error_free(err);
		return nullptr;
	}
	cairo_surface_t *s =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	RsvgRectangle vp = {0.0, 0.0, static_cast<double>(size),
			static_cast<double>(size)};
	gboolean ok = rsvg_handle_render_document(h, cr, &vp, &err);
	cairo_destroy(cr);
	g_object_unref(h);
	if (!ok) {
		if (err)
			g_error_free(err);
		cairo_surface_destroy(s);
		return nullptr;
	}
	cairo_surface_flush(s);
	return s;
}
#endif


}  // namespace

/* The user's selected icon theme: $WB_ICON_THEME, else the GTK setting
 * (gtk-icon-theme-name in gtk-4.0/gtk-3.0 settings.ini), else empty (the
 * resolver then uses hicolor). hicolor is always searched as the fallback. */
std::string detect_icon_theme() {
	if (const char *t = getenv("WB_ICON_THEME"); t && t[0] != '\0')
		return t;
	const char *home = getenv("HOME");
	const char *xdg = getenv("XDG_CONFIG_HOME");
	std::string cfg = (xdg && xdg[0]) ? std::string(xdg)
			: (home ? std::string(home) + "/.config" : std::string());
	if (cfg.empty())
		return {};
	for (const char *rel : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
		std::ifstream f(cfg + rel);
		std::string line;
		while (std::getline(f, line)) {
			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string key = line.substr(0, eq);
			while (!key.empty() && key.back() == ' ')
				key.pop_back();
			if (key == "gtk-icon-theme-name") {
				std::string val = line.substr(eq + 1);
				size_t a = val.find_first_not_of(" \t");
				size_t b = val.find_last_not_of(" \t\r");
				if (a != std::string::npos)
					return val.substr(a, b - a + 1);
			}
		}
	}
	return {};
}

/* A theme's parsed index.theme, read from the first base dir that has one
 * (null if none). Cached for the process: themes rarely change while running. */
static const IconThemeIndex *theme_index(const std::vector<std::string> &bases,
		std::string_view theme) {
	static std::map<std::string, std::unique_ptr<IconThemeIndex>, std::less<>> cache;
	if (auto it = cache.find(theme); it != cache.end())
		return it->second.get();
	std::unique_ptr<IconThemeIndex> index;
	for (const std::string &base : bases) {
		std::ifstream in(base + "/" + std::string(theme) + "/index.theme");
		if (!in)
			continue;
		std::string contents((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
		index = std::make_unique<IconThemeIndex>(parse_icon_theme_index(contents));
		break;
	}
	const IconThemeIndex *result = index.get();
	cache.emplace(std::string(theme), std::move(index));
	return result;
}

cairo_surface_t *load_themed_icon(const std::string &name, const std::string &theme,
		int size) {
	if (name.empty() || size <= 0)
		return nullptr;
	const char *home = getenv("HOME");
	const char *xdh = getenv("XDG_DATA_HOME");
	const char *xdd = getenv("XDG_DATA_DIRS");
	std::vector<std::string> bases = icon_base_dirs(home ? home : "",
			xdh ? xdh : "", xdd ? xdd : "");
	auto index_of = [&bases](std::string_view t) { return theme_index(bases, t); };
	for (const std::string &path : icon_search_paths(name, theme, size, bases, index_of)) {
		/* Most candidates don't exist: check cheaply before loading. Only
		 * regular files of sane size: reading a FIFO would block the
		 * compositor, a huge file would eat memory. */
		struct stat st;
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
				st.st_size > kMaxIconFileBytes || access(path.c_str(), R_OK) != 0)
			continue;
		if (ends_with(path, ".png")) {
			cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
			if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
				return s;
			cairo_surface_destroy(s);
		}
#ifdef HAVE_RSVG
		else if (ends_with(path, ".svg")) {
			if (cairo_surface_t *s = render_svg(path.c_str(), size))
				return s;
		}
#endif
	}
	return nullptr;
}

std::string desktop_file_icon(std::string_view app_id) {
	if (app_id.empty() || app_id.find('/') != std::string_view::npos)
		return {};
	std::vector<std::string> dirs;
	const char *home = getenv("HOME");
	const char *xdh = getenv("XDG_DATA_HOME");
	dirs.push_back((xdh && xdh[0]) ? std::string(xdh)
			: (home ? std::string(home) + "/.local/share" : std::string()));
	const char *xdd = getenv("XDG_DATA_DIRS");
	std::string list = (xdd && xdd[0]) ? xdd : "/usr/local/share:/usr/share";
	size_t start = 0;
	while (start <= list.size()) {
		size_t colon = list.find(':', start);
		std::string dir = list.substr(start, colon == std::string::npos
				? std::string::npos : colon - start);
		if (!dir.empty())
			dirs.push_back(dir);
		if (colon == std::string::npos)
			break;
		start = colon + 1;
	}
	std::string id(app_id), lower = id;
	std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return std::tolower(c); });
	for (const std::string &name : {id, lower}) {
		for (const std::string &dir : dirs) {
			if (dir.empty())
				continue;
			/* The name is the window's choice: a regular file only (a FIFO
			 * there would block the compositor), and not a huge one. */
			std::optional<std::string> contents =
					read_regular_file(dir + "/applications/" + name + ".desktop", 256 * 1024);
			if (!contents)
				continue;
			std::string icon = desktop_entry_icon(*contents);
			if (!icon.empty())
				return icon;
		}
	}
	return {};
}

}  // namespace wb
