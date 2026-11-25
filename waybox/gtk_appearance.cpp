#include "waybox/gtk_appearance.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace wb {

namespace {

std::vector<std::string> split(const std::string &s, char sep) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == sep) {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else if (c != ' ' && c != '\t' && c != '\r') {
			cur += c;
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

std::vector<std::string> data_dirs(const std::string &home, const std::string &xdg_data_home,
		const std::string &xdg_data_dirs, const char *sub, const char *home_dot) {
	std::vector<std::string> out;
	if (!home.empty())
		out.push_back(home + "/" + home_dot);
	out.push_back((xdg_data_home.empty() ? home + "/.local/share" : xdg_data_home) + "/" + sub);
	for (const std::string &d : split(xdg_data_dirs.empty()
			? std::string("/usr/local/share:/usr/share") : xdg_data_dirs, ':'))
		out.push_back(d + "/" + sub);
	return out;
}

/* Keys of the [Icon Theme] group. */
std::map<std::string, std::string> icon_theme_keys(const std::string &path) {
	std::map<std::string, std::string> keys;
	std::ifstream f(path);
	std::string line;
	bool in_group = false;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		if (line[0] == '[') {
			in_group = line == "[Icon Theme]";
			continue;
		}
		size_t eq = line.find('=');
		if (in_group && eq != std::string::npos) {
			std::string k = line.substr(0, eq), v = line.substr(eq + 1);
			while (!k.empty() && k.back() == ' ')
				k.pop_back();
			while (!v.empty() && v.front() == ' ')
				v.erase(0, 1);
			keys.emplace(k, v);
		}
	}
	return keys;
}

}  // namespace

std::vector<std::string> icon_theme_dirs(const std::string &home,
		const std::string &xdg_data_home, const std::string &xdg_data_dirs) {
	std::vector<std::string> out = data_dirs(home, xdg_data_home, xdg_data_dirs, "icons", ".icons");
	out.push_back("/usr/share/pixmaps");
	return out;
}

std::vector<std::string> gtk_theme_dirs(const std::string &home,
		const std::string &xdg_data_home, const std::string &xdg_data_dirs) {
	return data_dirs(home, xdg_data_home, xdg_data_dirs, "themes", ".themes");
}

std::map<std::string, IconTheme> find_icon_themes(const std::vector<std::string> &dirs) {
	std::map<std::string, IconTheme> out;
	for (const std::string &dir : dirs) {
		std::error_code ec;
		for (const auto &e : fs::directory_iterator(dir, ec)) {
			std::string id = e.path().filename().string();
			if (!e.is_directory(ec))
				continue;
			if (auto seen = out.find(id); seen != out.end()) {
				/* A real theme behind one of wbconf's stand-ins. */
				if (seen->second.stand_in && fs::exists(e.path() / "index.theme", ec) &&
						icon_theme_keys((e.path() / "index.theme").string())
								["X-Waybox-Stand-In"] != "true")
					seen->second.hides_real = true;
				continue;
			}
			IconTheme t;
			t.id = id;
			t.path = e.path().string();
			t.has_cursors = fs::is_directory(e.path() / "cursors", ec);
			bool index = fs::exists(e.path() / "index.theme", ec);
			if (!index && !t.has_cursors)
				continue;
			if (index) {
				auto keys = icon_theme_keys((e.path() / "index.theme").string());
				t.name = keys.count("Name") ? keys["Name"] : id;
				t.inherits = split(keys["Inherits"], ',');
				t.has_icons = !split(keys["Directories"], ',').empty();
				t.hidden = keys["Hidden"] == "true";
				t.stand_in = keys["X-Waybox-Stand-In"] == "true";
			} else {
				t.name = id;
			}
			out.emplace(id, std::move(t));
		}
	}
	return out;
}

std::vector<std::string> find_gtk_themes(const std::vector<std::string> &dirs) {
	std::set<std::string> out;
	for (const std::string &dir : dirs) {
		std::error_code ec;
		for (const auto &e : fs::directory_iterator(dir, ec))
			if (fs::is_directory(e.path() / "gtk-3.0", ec) ||
					fs::is_directory(e.path() / "gtk-4.0", ec))
				out.insert(e.path().filename().string());
	}
	return {out.begin(), out.end()};
}

std::vector<std::string> missing_fallbacks(const std::string &id,
		const std::map<std::string, IconTheme> &themes) {
	std::vector<std::string> missing;
	std::set<std::string> seen;
	std::vector<std::string> queue{id};
	for (size_t i = 0; i < queue.size(); ++i) {
		const std::string &name = queue[i];
		if (!seen.insert(name).second)
			continue;
		auto t = themes.find(name);
		if (t == themes.end()) {
			if (name != id || id == "hicolor")
				missing.push_back(name);
			continue;
		}
		for (const std::string &parent : t->second.inherits)
			queue.push_back(parent);
	}
	if (seen.count("hicolor") == 0 && themes.count("hicolor") == 0)
		missing.push_back("hicolor");
	/* The theme itself not being installed is a different problem. */
	if (themes.count(id) == 0 && id != "hicolor")
		missing.insert(missing.begin(), id);
	return missing;
}

std::string complete_icon_theme(const std::map<std::string, IconTheme> &themes) {
	for (const char *pref : {"Adwaita", "breeze"}) {
		auto t = themes.find(pref);
		if (t != themes.end() && t->second.has_icons && !t->second.stand_in &&
				missing_fallbacks(pref, themes).empty())
			return pref;
	}
	for (const auto &[id, t] : themes)
		if (t.has_icons && !t.hidden && !t.stand_in && id != "hicolor" &&
				missing_fallbacks(id, themes).empty())
			return id;
	return "";
}

std::string stand_in_theme(const std::string &missing, const std::string &target) {
	std::ostringstream s;
	s << "[Icon Theme]\n"
	  << "Name=" << missing << "\n"
	  << "Comment=A stand-in made by wbconf: another theme falls back to " << missing
	  << ", which isn't installed, so its missing icons come from " << target << ".\n"
	  << "Inherits=" << target << ",hicolor\n"
	  << "Directories=\n"
	  << "Hidden=true\n"
	  << "X-Waybox-Stand-In=true\n";
	return s.str();
}

}  // namespace wb
