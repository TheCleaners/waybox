#include "waybox/xcursor_themes.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace wb {

namespace {

std::string expand_home(const std::string &dir, const std::string &home) {
	if (dir == "~")
		return home;
	if (dir.rfind("~/", 0) == 0)
		return home + dir.substr(1);
	return dir;
}

/* A theme's own cursors: a cursors/ directory with something in it. */
bool own_cursors(const fs::path &theme_dir) {
	std::error_code ec;
	fs::directory_iterator it(theme_dir / "cursors", ec);
	return !ec && it != fs::directory_iterator();
}

/* Inherits= from an index.theme, split on ',' ';' and blanks (as
 * libXcursor does). Present-but-empty gives an empty list. */
std::vector<std::string> read_inherits(const fs::path &index) {
	std::ifstream in(index);
	std::string line;
	while (std::getline(in, line)) {
		if (line.rfind("Inherits", 0) != 0)
			continue;
		size_t i = 8;
		while (i < line.size() && line[i] == ' ')
			++i;
		if (i >= line.size() || line[i] != '=')
			continue;
		std::vector<std::string> out;
		std::string cur;
		for (char c : line.substr(i + 1)) {
			if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r') {
				if (!cur.empty())
					out.push_back(cur);
				cur.clear();
			} else {
				cur += c;
			}
		}
		if (!cur.empty())
			out.push_back(cur);
		return out;
	}
	return {};
}

bool check(const std::string &theme, const std::vector<std::string> &path,
		std::set<std::string> &visited, XcursorThemeCheck *top) {
	if (!visited.insert(theme).second)
		return false;
	bool found = false;
	bool have_index = false;
	std::vector<std::string> inherits;
	std::error_code ec;
	for (const std::string &dir : path) {
		fs::path d = fs::path(dir) / theme;
		if (own_cursors(d))
			found = true;
		/* Only the first index.theme counts, as in libXcursor. */
		if (!have_index && fs::is_regular_file(d / "index.theme", ec)) {
			have_index = true;
			inherits = read_inherits(d / "index.theme");
			if (top != nullptr) {
				top->index_file = (d / "index.theme").string();
				top->inherits = inherits;
			}
		}
	}
	if (found)
		return true;
	for (const std::string &parent : inherits)
		if (check(parent, path, visited, nullptr))
			return true;
	return false;
}

}  // namespace

std::vector<std::string> xcursor_search_path(const char *xcursor_path, const char *xdg_data_home,
		const std::string &home) {
	std::vector<std::string> out;
	auto add = [&](const std::string &d) {
		if (!d.empty())
			out.push_back(expand_home(d, home));
	};
	if (xcursor_path != nullptr) {
		std::string s = xcursor_path;
		size_t start = 0;
		while (start <= s.size()) {
			size_t colon = s.find(':', start);
			add(s.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
			if (colon == std::string::npos)
				break;
			start = colon + 1;
		}
		return out;
	}
	std::string data_home = xdg_data_home != nullptr && xdg_data_home[0] == '/'
			? xdg_data_home : "~/.local/share";
	for (const std::string &d : {data_home + "/icons", std::string("~/.icons"),
			std::string("/usr/share/icons"), std::string("/usr/share/pixmaps"),
			std::string("~/.cursors"), std::string("/usr/share/cursors/xorg-x11"),
			std::string("/usr/local/share/icons")})
		add(d);
	return out;
}

XcursorThemeCheck xcursor_theme_check(const std::string &theme,
		const std::vector<std::string> &path) {
	XcursorThemeCheck result;
	std::set<std::string> visited;
	result.has_cursors = check(theme.empty() ? "default" : theme, path, visited, &result);
	return result;
}

std::vector<std::string> xcursor_themes_with_cursors(const std::vector<std::string> &path) {
	std::set<std::string> names;
	std::error_code ec;
	for (const std::string &dir : path)
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			if (own_cursors(it->path()))
				names.insert(it->path().filename().string());
	return {names.begin(), names.end()};
}

std::string xcursor_fallback_theme(const std::vector<std::string> &path) {
	std::vector<std::string> themes = xcursor_themes_with_cursors(path);
	if (std::find(themes.begin(), themes.end(), "Adwaita") != themes.end())
		return "Adwaita";
	return themes.empty() ? std::string() : themes.front();
}

}  // namespace wb
