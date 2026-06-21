/*
 * Pure XDG icon-theme path resolution. No filesystem access (candidate paths
 * only) and no wlroots/Cairo, so it links into the standalone unit test.
 */
#include "waybox/icon.hpp"

#include <array>
#include <filesystem>

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
	std::vector<std::string> paths;
	if (name.empty())
		return paths;
	if (is_absolute(name)) {
		paths.emplace_back(name);
		return paths;
	}

	static constexpr std::array<const char *, 3> kExts = {"png", "svg", "xpm"};
	/* Common contexts an app/menu icon might live under. */
	static constexpr std::array<const char *, 5> kContexts = {
			"apps", "categories", "places", "devices", "actions"};

	/* Preferred size, then a couple of larger common sizes, then scalable. */
	std::vector<std::string> size_dirs;
	size_dirs.push_back(std::to_string(size) + "x" + std::to_string(size));
	for (int s : {48, 64, 128, 256}) {
		std::string d = std::to_string(s) + "x" + std::to_string(s);
		if (d != size_dirs.front())
			size_dirs.push_back(d);
	}
	size_dirs.push_back("scalable");

	/* Try the named theme, then the hicolor fallback. */
	std::vector<std::string> themes;
	themes.emplace_back(theme.empty() ? "hicolor" : std::string(theme));
	if (themes.front() != "hicolor")
		themes.emplace_back("hicolor");

	const std::string nm(name);
	for (const std::string &base : base_dirs) {
		/* The pixmaps base is flat (no theme/size structure). */
		bool flat = base.size() >= 8 &&
				base.compare(base.size() - 8, 8, "/pixmaps") == 0;
		if (flat) {
			for (const char *ext : kExts)
				paths.push_back(base + "/" + nm + "." + ext);
			continue;
		}
		for (const std::string &th : themes) {
			for (const std::string &sd : size_dirs) {
				for (const char *ctx : kContexts) {
					for (const char *ext : kExts) {
						paths.push_back(base + "/" + th + "/" + sd + "/" + ctx +
								"/" + nm + "." + ext);
					}
				}
			}
		}
	}
	return paths;
}

}  // namespace wb
