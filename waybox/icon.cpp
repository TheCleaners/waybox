/*
 * Pure XDG icon-theme path resolution. No filesystem access (candidate paths
 * only) and no wlroots/Cairo, so it links into the standalone unit test.
 */
#include "waybox/icon.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>

namespace wb {

namespace {

namespace fs = std::filesystem;

bool is_absolute(std::string_view name) {
	return !name.empty() && name.front() == '/';
}

/* Split a colon-separated path list into entries, skipping empties. */
std::vector<std::string> split_dirs(std::string_view dirs) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= dirs.size()) {
		size_t colon = dirs.find(':', pos);
		std::string_view part = dirs.substr(
				pos, colon == std::string_view::npos ? std::string_view::npos
				                                      : colon - pos);
		if (!part.empty())
			out.emplace_back(part);
		if (colon == std::string_view::npos)
			break;
		pos = colon + 1;
	}
	return out;
}

}  // namespace

std::vector<std::string> icon_base_dirs(std::string_view home,
		std::string_view xdg_data_home, std::string_view xdg_data_dirs) {
	std::vector<std::string> bases;

	if (!xdg_data_home.empty())
		bases.push_back((fs::path(xdg_data_home) / "icons").string());
	else if (!home.empty())
		bases.push_back((fs::path(home) / ".local" / "share" / "icons").string());

	if (!home.empty())
		bases.push_back((fs::path(home) / ".icons").string());

	std::string dirs = xdg_data_dirs.empty()
			? "/usr/local/share:/usr/share"
			: std::string(xdg_data_dirs);
	for (const std::string &d : split_dirs(dirs))
		bases.push_back((fs::path(d) / "icons").string());

	/* Legacy fallback location for un-themed icons. */
	bases.push_back("/usr/share/pixmaps");
	return bases;
}

std::vector<std::string> icon_search_paths(std::string_view name,
		std::string_view theme, int size,
		const std::vector<std::string> &base_dirs) {
	/* Without theme indexes: the generic layout for the theme and hicolor. */
	return icon_search_paths(name, theme, size, base_dirs, nullptr);
}

namespace {

constexpr std::array<const char *, 3> kExts = {"png", "svg", "xpm"};

bool is_pixmaps(const std::string &base) {
	return base.size() >= 8 &&
			base.compare(base.size() - 8, 8, "/pixmaps") == 0;
}

/* The generic layout guessed for themes without an index.theme. */
std::vector<std::string> generic_dirs_for_size(int size) {
	static constexpr std::array<const char *, 5> kContexts = {
			"apps", "categories", "places", "devices", "actions"};
	std::vector<std::string> size_dirs;
	size_dirs.push_back(std::to_string(size) + "x" + std::to_string(size));
	for (int s : {48, 64, 128, 256}) {
		std::string d = std::to_string(s) + "x" + std::to_string(s);
		if (d != size_dirs.front())
			size_dirs.push_back(d);
	}
	size_dirs.push_back("scalable");
	std::vector<std::string> dirs;
	for (const std::string &sd : size_dirs)
		for (const char *ctx : kContexts)
			dirs.push_back(sd + "/" + ctx);
	return dirs;
}

std::string trim(std::string_view s) {
	size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string_view::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r");
	return std::string(s.substr(a, b - a + 1));
}

std::vector<std::string> split_list(std::string_view s, char sep) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t next = s.find(sep, pos);
		std::string item = trim(s.substr(pos, next == std::string_view::npos
				? std::string_view::npos : next - pos));
		if (!item.empty())
			out.push_back(item);
		if (next == std::string_view::npos)
			break;
		pos = next + 1;
	}
	return out;
}

int to_int(const std::string &s, int fallback) {
	try {
		return std::stoi(s);
	} catch (...) {
		return fallback;
	}
}

bool dir_matches(const IconThemeDir &d, int size) {
	switch (d.type) {
	case IconThemeDir::Type::Fixed:
		return d.size == size;
	case IconThemeDir::Type::Scalable:
		return d.min_size <= size && size <= d.max_size;
	case IconThemeDir::Type::Threshold:
	default:
		return d.size - d.threshold <= size && size <= d.size + d.threshold;
	}
}

int dir_distance(const IconThemeDir &d, int size) {
	switch (d.type) {
	case IconThemeDir::Type::Fixed:
		return d.size > size ? d.size - size : size - d.size;
	case IconThemeDir::Type::Scalable:
		if (size < d.min_size)
			return d.min_size - size;
		if (size > d.max_size)
			return size - d.max_size;
		return 0;
	case IconThemeDir::Type::Threshold:
	default:
		if (size < d.size - d.threshold)
			return d.min_size - size;
		if (size > d.size + d.threshold)
			return size - d.max_size;
		return 0;
	}
}

}  // namespace

IconThemeIndex parse_icon_theme_index(std::string_view contents) {
	IconThemeIndex index;
	std::vector<std::string> listed;
	/* section name -> key -> value */
	std::vector<std::pair<std::string, std::vector<std::pair<std::string,
			std::string>>>> sections;
	std::string section;
	size_t pos = 0;
	while (pos < contents.size()) {
		size_t eol = contents.find('\n', pos);
		std::string_view line = contents.substr(pos,
				eol == std::string_view::npos ? std::string_view::npos : eol - pos);
		pos = eol == std::string_view::npos ? contents.size() : eol + 1;
		std::string l = trim(line);
		if (l.empty() || l[0] == '#')
			continue;
		if (l.front() == '[' && l.back() == ']') {
			section = l.substr(1, l.size() - 2);
			sections.push_back({section, {}});
			continue;
		}
		size_t eq = l.find('=');
		if (eq == std::string::npos || sections.empty())
			continue;
		std::string key = trim(std::string_view(l).substr(0, eq));
		std::string val = trim(std::string_view(l).substr(eq + 1));
		if (section == "Icon Theme") {
			if (key == "Directories" || key == "ScaledDirectories") {
				for (std::string &d : split_list(val, ','))
					listed.push_back(std::move(d));
			} else if (key == "Inherits") {
				index.inherits = split_list(val, ',');
			}
		}
		sections.back().second.push_back({key, val});
	}
	for (const std::string &name : listed) {
		IconThemeDir d;
		d.path = name;
		for (const auto &[sec, keys] : sections) {
			if (sec != name)
				continue;
			for (const auto &[k, v] : keys) {
				/* Bounded, so size +/- threshold can't overflow. */
				auto px = [&](int fallback) {
					return std::clamp(to_int(v, fallback), 0, 4096);
				};
				if (k == "Size")
					d.size = px(0);
				else if (k == "MinSize")
					d.min_size = px(0);
				else if (k == "MaxSize")
					d.max_size = px(0);
				else if (k == "Threshold")
					d.threshold = px(2);
				else if (k == "Scale")
					d.scale = std::clamp(to_int(v, 1), 1, 16);
				else if (k == "Type")
					d.type = v == "Fixed" ? IconThemeDir::Type::Fixed
							: v == "Scalable" ? IconThemeDir::Type::Scalable
							: IconThemeDir::Type::Threshold;
			}
		}
		if (d.size <= 0)
			continue;  /* unusable without a size */
		if (d.min_size <= 0)
			d.min_size = d.size;
		if (d.max_size <= 0)
			d.max_size = d.size;
		index.dirs.push_back(std::move(d));
	}
	return index;
}

std::vector<std::string> icon_theme_dirs_for_size(const IconThemeIndex &index,
		int size) {
	std::vector<const IconThemeDir *> order;
	for (const IconThemeDir &d : index.dirs)
		order.push_back(&d);
	std::stable_sort(order.begin(), order.end(),
			[size](const IconThemeDir *a, const IconThemeDir *b) {
		if ((a->scale == 1) != (b->scale == 1))
			return a->scale == 1;
		bool ma = dir_matches(*a, size), mb = dir_matches(*b, size);
		if (ma != mb)
			return ma;
		int da = dir_distance(*a, size), db = dir_distance(*b, size);
		if (da != db)
			return da < db;
		return a->size > b->size;  /* prefer downscaling to upscaling */
	});
	std::vector<std::string> out;
	for (const IconThemeDir *d : order)
		out.push_back(d->path);
	return out;
}

std::vector<std::string> icon_search_paths(std::string_view name,
		std::string_view theme, int size,
		const std::vector<std::string> &base_dirs,
		const IconThemeIndexLookup &index_of) {
	std::vector<std::string> paths;
	if (name.empty())
		return paths;
	if (is_absolute(name)) {
		paths.emplace_back(name);
		return paths;
	}

	/* Theme chain: the theme, its parents depth first (each once), hicolor. */
	std::vector<std::string> chain;
	std::function<void(const std::string &)> add = [&](const std::string &t) {
		if (t.empty() || t == "hicolor" ||
				std::find(chain.begin(), chain.end(), t) != chain.end())
			return;
		chain.push_back(t);
		if (index_of) {
			if (const IconThemeIndex *idx = index_of(t))
				for (const std::string &parent : idx->inherits)
					add(parent);
		}
	};
	add(std::string(theme));
	chain.push_back("hicolor");

	const std::string nm(name);
	for (const std::string &th : chain) {
		const IconThemeIndex *idx = index_of ? index_of(th) : nullptr;
		std::vector<std::string> dirs = idx != nullptr
				? icon_theme_dirs_for_size(*idx, size)
				: generic_dirs_for_size(size);
		for (const std::string &dir : dirs) {
			for (const std::string &base : base_dirs) {
				if (is_pixmaps(base))
					continue;
				for (const char *ext : kExts)
					paths.push_back(base + "/" + th + "/" + dir + "/" + nm + "." + ext);
			}
		}
	}
	/* The pixmaps base is flat (no theme/size structure). */
	for (const std::string &base : base_dirs) {
		if (is_pixmaps(base))
			for (const char *ext : kExts)
				paths.push_back(base + "/" + nm + "." + ext);
	}
	return paths;
}

std::string desktop_entry_icon(std::string_view contents) {
	bool in_entry = false;
	size_t pos = 0;
	while (pos <= contents.size()) {
		size_t nl = contents.find('\n', pos);
		std::string_view line = contents.substr(pos, nl == std::string_view::npos
				? std::string_view::npos : nl - pos);
		pos = nl == std::string_view::npos ? contents.size() + 1 : nl + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.remove_suffix(1);
		if (!line.empty() && line.front() == '[') {
			in_entry = line == "[Desktop Entry]";
			continue;
		}
		if (!in_entry || line.substr(0, 4) != "Icon")
			continue;
		std::string_view rest = line.substr(4);
		while (!rest.empty() && rest.front() == ' ')
			rest.remove_prefix(1);
		if (rest.empty() || rest.front() != '=')
			continue;  /* Icon[xx]= and the like */
		rest.remove_prefix(1);
		while (!rest.empty() && rest.front() == ' ')
			rest.remove_prefix(1);
		return std::string(rest);
	}
	return {};
}

}  // namespace wb
