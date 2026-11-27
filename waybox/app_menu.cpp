#include "waybox/atomic_file.hpp"
#include "waybox/app_menu.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/action.hpp"

namespace wb {

namespace {

constexpr size_t kMaxFileBytes = 256 * 1024;
constexpr size_t kMaxEntries = 4000;
constexpr int kMaxDepth = 4;

std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

/* The spec's string escapes: \s \n \t \r \\ (and \; kept for lists). */
std::string unescape(std::string_view v, bool keep_semicolon) {
	std::string out;
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] != '\\' || i + 1 == v.size()) {
			out += v[i];
			continue;
		}
		char c = v[++i];
		switch (c) {
		case 's': out += ' '; break;
		case 'n': out += '\n'; break;
		case 't': out += '\t'; break;
		case 'r': out += '\r'; break;
		case '\\': out += '\\'; break;
		case ';':
			if (keep_semicolon)
				out += '\\';
			out += ';';
			break;
		default: out += '\\'; out += c; break;
		}
	}
	return out;
}

std::vector<std::string> split_list(std::string_view v) {
	std::vector<std::string> out;
	std::string cur;
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == ';') {
			cur += ';';
			++i;
		} else if (v[i] == ';') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else {
			cur += v[i];
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

/* Exec without its field codes (%f %U %i ...; %% is a percent). */
std::string strip_field_codes(std::string_view exec) {
	std::string out;
	for (size_t i = 0; i < exec.size(); ++i) {
		if (exec[i] == '%' && i + 1 < exec.size()) {
			if (exec[i + 1] == '%')
				out += '%';
			++i;
			continue;
		}
		out += exec[i];
	}
	return std::string(trim(out));
}

bool is_true(std::string_view v) { return trim(v) == "true"; }

std::string shell_quote(std::string_view s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'')
			out += "'\\''";
		else
			out += c;
	}
	return out + "'";
}

std::string lower(std::string_view s) {
	std::string out(s);
	for (char &c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

struct Category {
	const char *key, *label, *icon;
};

/* The XDG main categories, as menus usually name them. */
constexpr Category kCategories[] = {
	{"AudioVideo", "Multimedia", "applications-multimedia"},
	{"Development", "Development", "applications-development"},
	{"Education", "Education", "applications-education"},
	{"Game", "Games", "applications-games"},
	{"Graphics", "Graphics", "applications-graphics"},
	{"Network", "Internet", "applications-internet"},
	{"Office", "Office", "applications-office"},
	{"Science", "Science", "applications-science"},
	{"Settings", "Settings", "preferences-system"},
	{"System", "System", "applications-system"},
	{"Utility", "Accessories", "applications-accessories"},
	{"Other", "Other", "applications-other"},
};

/* Whether `name` runs: a path that's executable, or a program on $PATH. */
bool program_found(const std::string &name) {
	if (name.empty())
		return true;
	if (name.find('/') != std::string::npos)
		return access(name.c_str(), X_OK) == 0;
	const char *path = getenv("PATH");
	std::string_view rest = path ? path : "/usr/local/bin:/usr/bin:/bin";
	while (true) {
		size_t colon = rest.find(':');
		std::string dir(rest.substr(0, colon));
		if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0)
			return true;
		if (colon == std::string_view::npos)
			return false;
		rest.remove_prefix(colon + 1);
	}
}

bool read_small_file(const std::string &path, std::string &out) {
	/* Opened without blocking and read only if a regular file: one swapped
	 * for a FIFO after the scan's stat can't stall the menu. */
	std::optional<std::string> text = read_regular_file(path, kMaxFileBytes);
	if (!text)
		return false;
	out = std::move(*text);
	return true;
}

void scan_dir(const std::string &dir, const std::string &prefix, int depth,
		const std::vector<std::string> &langs, std::set<std::string> &seen,
		std::vector<DesktopEntry> &all) {
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return;
	std::vector<std::string> names;
	while (struct dirent *e = readdir(d))
		if (e->d_name[0] != '.')
			names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	for (const std::string &name : names) {
		if (all.size() >= kMaxEntries)
			return;
		std::string path = dir + "/" + name;
		struct stat st {};
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < kMaxDepth)
				scan_dir(path, prefix + name + "-", depth + 1, langs, seen, all);
			continue;
		}
		if (!S_ISREG(st.st_mode) || name.size() <= 8 ||
				name.compare(name.size() - 8, 8, ".desktop") != 0)
			continue;
		std::string id = prefix + name;
		if (!seen.insert(id).second)
			continue;  /* shadowed by an earlier directory */
		std::string text;
		DesktopEntry entry;
		if (!read_small_file(path, text) || !parse_desktop_entry(text, langs, entry))
			continue;
		entry.id = id;
		all.push_back(std::move(entry));
	}
}

/* Every directory under `dir` (itself included), for change stamps. */
void list_dirs(const std::string &dir, int depth, std::vector<std::string> &out) {
	out.push_back(dir);
	if (depth >= kMaxDepth)
		return;
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return;
	std::vector<std::string> subs;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] == '.')
			continue;
		std::string path = dir + "/" + e->d_name;
		struct stat st {};
		if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			subs.push_back(path);
	}
	closedir(d);
	for (const std::string &s : subs)
		list_dirs(s, depth + 1, out);
}

struct timespec mtime_of(const std::string &dir) {
	struct stat st {};
	if (stat(dir.c_str(), &st) != 0)
		return {-1, 0};
#if defined(__APPLE__)
	return st.st_mtimespec;
#else
	return st.st_mtim;
#endif
}

}  // namespace

std::vector<std::string> locale_names(std::string_view locale) {
	std::string_view l = locale;
	if (l.empty() || l == "C" || l == "POSIX" || l.rfind("C.", 0) == 0)
		return {};
	std::string modifier;
	if (size_t at = l.find('@'); at != std::string_view::npos) {
		modifier = std::string(l.substr(at));
		l = l.substr(0, at);
	}
	if (size_t dot = l.find('.'); dot != std::string_view::npos)
		l = l.substr(0, dot);
	std::string lang(l), country;
	if (size_t us = l.find('_'); us != std::string_view::npos) {
		lang = std::string(l.substr(0, us));
		country = std::string(l.substr(us));
	}
	std::vector<std::string> out;
	if (!country.empty() && !modifier.empty())
		out.push_back(lang + country + modifier);
	if (!country.empty())
		out.push_back(lang + country);
	if (!modifier.empty())
		out.push_back(lang + modifier);
	out.push_back(lang);
	return out;
}

bool parse_desktop_entry(std::string_view text, const std::vector<std::string> &langs,
		DesktopEntry &e) {
	bool in_entry = false, seen = false;
	size_t best_name = langs.size() + 1;  /* rank of the Name kept (langs.size(): plain) */
	std::istringstream in{std::string(text)};
	std::string raw;
	while (std::getline(in, raw)) {
		std::string_view l = trim(raw);
		if (l.empty() || l[0] == '#')
			continue;
		if (l[0] == '[') {
			if (seen && in_entry)
				break;  /* past [Desktop Entry]: actions and the rest */
			in_entry = l == "[Desktop Entry]";
			seen = seen || in_entry;
			continue;
		}
		if (!in_entry)
			continue;
		size_t eq = l.find('=');
		if (eq == std::string_view::npos)
			continue;
		std::string_view key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
		if (key.rfind("Name", 0) == 0 && (key.size() == 4 || key[4] == '[')) {
			size_t rank = langs.size();
			if (key.size() > 4) {
				if (key.back() != ']')
					continue;
				std::string_view lang = key.substr(5, key.size() - 6);
				auto it = std::find(langs.begin(), langs.end(), lang);
				if (it == langs.end())
					continue;
				rank = static_cast<size_t>(it - langs.begin());
			}
			if (rank < best_name) {
				e.name = unescape(value, false);
				best_name = rank;
			}
		} else if (key == "Type") {
			e.application = value == "Application";
		} else if (key == "Exec") {
			e.exec = strip_field_codes(unescape(value, false));
		} else if (key == "Icon") {
			e.icon = unescape(value, false);
		} else if (key == "Path") {
			e.path = unescape(value, false);
		} else if (key == "TryExec") {
			e.try_exec = unescape(value, false);
		} else if (key == "Categories") {
			e.categories = split_list(unescape(value, true));
		} else if (key == "OnlyShowIn") {
			e.only_show_in = split_list(unescape(value, true));
		} else if (key == "NotShowIn") {
			e.not_show_in = split_list(unescape(value, true));
		} else if (key == "Terminal") {
			e.terminal = is_true(value);
		} else if (key == "NoDisplay") {
			e.no_display = is_true(value);
		} else if (key == "Hidden") {
			e.hidden = is_true(value);
		}
	}
	return seen;
}

bool desktop_entry_shown(const DesktopEntry &e, const std::vector<std::string> &desktops) {
	if (!e.application || e.exec.empty() || e.name.empty() || e.no_display || e.hidden)
		return false;
	auto here = [&desktops](const std::vector<std::string> &list) {
		for (const std::string &d : list)
			if (std::find(desktops.begin(), desktops.end(), d) != desktops.end())
				return true;
		return false;
	};
	return (e.only_show_in.empty() || here(e.only_show_in)) && !here(e.not_show_in);
}

std::string main_category(const DesktopEntry &e) {
	/* The first main category listed (the spec's order of preference);
	 * Audio or Video alone count as AudioVideo. */
	for (const std::string &c : e.categories) {
		if (c == "Audio" || c == "Video")
			return "AudioVideo";
		for (const Category &k : kCategories)
			if (c == k.key)
				return c;
	}
	return "Other";
}

std::string desktop_entry_command(const DesktopEntry &e, std::string_view terminal) {
	std::string cmd = e.exec;
	if (e.terminal)
		cmd = std::string(terminal) + " -e " + cmd;
	if (!e.path.empty())
		cmd = "cd " + shell_quote(e.path) + " && " + cmd;
	return cmd;
}

MenuFile build_applications_menu(const std::string &id, const std::string &label,
		const std::vector<DesktopEntry> &entries, std::string_view terminal) {
	std::map<std::string, std::vector<const DesktopEntry *>> by_category;
	for (const DesktopEntry &e : entries)
		by_category[main_category(e)].push_back(&e);
	MenuFile file;
	Menu root;
	root.id = id;
	root.label = label;
	std::vector<const Category *> cats;
	for (const Category &c : kCategories)
		if (by_category.count(c.key))
			cats.push_back(&c);
	/* By label, "Other" last. */
	std::sort(cats.begin(), cats.end(), [](const Category *a, const Category *b) {
		bool ao = std::string_view(a->key) == "Other", bo = std::string_view(b->key) == "Other";
		if (ao != bo)
			return bo;
		return lower(a->label) < lower(b->label);
	});
	for (const Category *c : cats) {
		std::vector<const DesktopEntry *> list = by_category[c->key];
		std::sort(list.begin(), list.end(), [](const DesktopEntry *a, const DesktopEntry *b) {
			std::string la = lower(a->name), lb = lower(b->name);
			return la != lb ? la < lb : a->id < b->id;
		});
		Menu sub;
		sub.id = id + "/" + c->key;
		sub.label = c->label;
		for (const DesktopEntry *e : list) {
			MenuItem item;
			item.kind = MenuItem::Kind::Entry;
			item.label = e->name;
			item.icon = e->icon;
			if (auto action = make_action("Execute", desktop_entry_command(*e, terminal)))
				item.actions.push_back(*action);
			sub.items.push_back(std::move(item));
		}
		MenuItem link;
		link.kind = MenuItem::Kind::Submenu;
		link.label = c->label;
		link.icon = c->icon;
		link.submenu_id = sub.id;
		root.items.push_back(std::move(link));
		file.menus.push_back(std::move(sub));
	}
	file.menus.push_back(std::move(root));
	return file;
}

std::vector<std::string> application_dirs(const char *data_home, const char *home,
		const char *data_dirs) {
	std::vector<std::string> out;
	if (data_home != nullptr && data_home[0] == '/')
		out.push_back(std::string(data_home) + "/applications");
	else if (home != nullptr && home[0] == '/')
		out.push_back(std::string(home) + "/.local/share/applications");
	std::string_view rest = data_dirs != nullptr && data_dirs[0] != '\0' ? data_dirs
			: "/usr/local/share:/usr/share";
	while (true) {
		size_t colon = rest.find(':');
		std::string dir(rest.substr(0, colon));
		if (!dir.empty() && dir[0] == '/') {
			while (dir.size() > 1 && dir.back() == '/')
				dir.pop_back();
			std::string apps = dir + "/applications";
			if (std::find(out.begin(), out.end(), apps) == out.end())
				out.push_back(apps);
		}
		if (colon == std::string_view::npos)
			break;
		rest.remove_prefix(colon + 1);
	}
	return out;
}

std::vector<DesktopEntry> scan_applications(const std::vector<std::string> &dirs,
		const std::vector<std::string> &langs, const std::vector<std::string> &desktops) {
	std::set<std::string> seen;
	std::vector<DesktopEntry> all;
	for (const std::string &dir : dirs)
		scan_dir(dir, "", 0, langs, seen, all);
	std::vector<DesktopEntry> shown;
	for (DesktopEntry &e : all)
		if (desktop_entry_shown(e, desktops) && program_found(e.try_exec))
			shown.push_back(std::move(e));
	return shown;
}

const MenuFile &ApplicationsMenu::get(const std::string &id, const std::string &label,
		const std::vector<std::string> &dirs, const std::vector<std::string> &langs,
		const std::vector<std::string> &desktops, const std::string &terminal) {
	std::string key = id + '\n' + label + '\n' + terminal;
	for (const std::string &d : dirs)
		key += '\n' + d;
	for (const std::string &l : langs)
		key += "\nL" + l;
	for (const std::string &d : desktops)
		key += "\nD" + d;
	bool fresh = !stamps_.empty() && key == key_;
	for (const Stamp &s : stamps_) {
		struct timespec now = mtime_of(s.first);
		if (now.tv_sec != s.second.tv_sec || now.tv_nsec != s.second.tv_nsec) {
			fresh = false;
			break;
		}
	}
	if (fresh)
		return menu_;
	/* Stamp first: a change while reading shows next time. */
	stamps_.clear();
	for (const std::string &dir : dirs) {
		std::vector<std::string> all;
		list_dirs(dir, 0, all);
		for (const std::string &d : all)
			stamps_.push_back({d, mtime_of(d)});
	}
	key_ = key;
	menu_ = build_applications_menu(id, label, scan_applications(dirs, langs, desktops),
			terminal);
	return menu_;
}

}  // namespace wb
