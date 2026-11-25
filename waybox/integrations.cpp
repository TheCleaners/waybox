/*
 * Integrations (see integrations.hpp).
 */
#include "waybox/integrations.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "waybox/startup.hpp"

namespace fs = std::filesystem;

namespace wb {

namespace {

std::string_view trim(std::string_view s) {
	size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string_view::npos)
		return {};
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

int category_rank(const std::string &c) {
	static const char *const order[] = {"Panel", "Notifications", "Wallpaper",
			"NightLight", "Idle", "Clipboard", "Pickers", "Tray", "Polkit", "Displays", nullptr};
	for (int i = 0; order[i] != nullptr; ++i)
		if (c == order[i])
			return i;
	return 100;
}

}  // namespace

std::optional<Integration> parse_integration(std::string_view id, std::string_view text) {
	Integration in;
	in.id = std::string(id);
	std::string section;
	IntegrationOption *opt = nullptr;
	std::istringstream lines{std::string(text)};
	std::string raw;
	while (std::getline(lines, raw)) {
		std::string_view line = trim(raw);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			section = std::string(trim(line.substr(1, line.size() - 2)));
			opt = nullptr;
			if (section.rfind("Option ", 0) == 0) {
				in.options.push_back({std::string(trim(section.substr(7))), "", "", ""});
				opt = &in.options.back();
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string_view::npos)
			continue;
		std::string key(trim(line.substr(0, eq)));
		std::string value(trim(line.substr(eq + 1)));
		if (section == "Integration") {
			if (key == "Name") in.name = value;
			else if (key == "Category") in.category = value;
			else if (key == "Comment") in.comment = value;
			else if (key == "Exec") in.exec = value;
			else if (key == "Binary") in.binary = value;
			else if (key == "Kind") {
				in.binding = value == "binding";
				in.input_method = value == "inputmethod";
			} else if (key == "Env") {
				size_t eq = value.find('=');
				if (eq != std::string::npos && eq > 0)
					in.env.emplace_back(value.substr(0, eq), value.substr(eq + 1));
			}
			else if (key == "Key") in.key = value;
		} else if (opt != nullptr) {
			if (key == "Label") opt->label = value;
			else if (key == "Flag") opt->flag = value;
			else if (key == "Hint") opt->hint = value;
		}
	}
	if (in.name.empty() || in.exec.empty())
		return std::nullopt;
	if (in.binary.empty())
		in.binary = startup_name("", in.exec);
	if (in.category.empty())
		in.category = "Other";
	/* Options need a flag to be read and written. */
	in.options.erase(std::remove_if(in.options.begin(), in.options.end(),
			[](const IntegrationOption &o) { return o.flag.empty() || o.id.empty(); }),
			in.options.end());
	for (IntegrationOption &o : in.options)
		if (o.label.empty())
			o.label = o.id;
	return in;
}

std::vector<Integration> load_integrations(const std::vector<std::string> &dirs) {
	std::vector<Integration> out;
	std::set<std::string> seen;
	for (const std::string &dir : dirs) {
		std::error_code ec;
		std::vector<fs::path> files;
		fs::directory_iterator it(dir, ec), end;
		for (; !ec && it != end; it.increment(ec))
			if (it->path().extension() == ".integration")
				files.push_back(it->path());
		std::sort(files.begin(), files.end());
		for (const fs::path &f : files) {
			std::string id = f.stem().string();
			if (seen.count(id))
				continue;
			std::ifstream in(f);
			std::stringstream text;
			text << in.rdbuf();
			if (auto parsed = parse_integration(id, text.str())) {
				seen.insert(id);
				out.push_back(std::move(*parsed));
			}
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const Integration &a, const Integration &b) {
		int ra = category_rank(a.category), rb = category_rank(b.category);
		if (ra != rb)
			return ra < rb;
		if (a.category != b.category)
			return a.category < b.category;
		return a.name < b.name;
	});
	return out;
}

std::vector<std::string> integration_dirs(const std::string &home,
		const std::string &xdg_config_home, const std::string &xdg_data_dirs,
		const std::string &installed) {
	std::vector<std::string> dirs;
	auto add = [&](const std::string &d) {
		if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
			dirs.push_back(d);
	};
	if (!xdg_config_home.empty())
		add(xdg_config_home + "/waybox/integrations");
	else if (!home.empty())
		add(home + "/.config/waybox/integrations");
	std::string data_dirs = xdg_data_dirs.empty() ? "/usr/local/share:/usr/share" : xdg_data_dirs;
	std::stringstream ss(data_dirs);
	std::string d;
	while (std::getline(ss, d, ':'))
		if (!d.empty())
			add(d + "/waybox/integrations");
	add(installed);
	return dirs;
}

std::vector<std::string> split_command(std::string_view command) {
	std::vector<std::string> words;
	std::string cur;
	bool in_word = false;
	char quote = 0;
	for (size_t i = 0; i < command.size(); ++i) {
		char c = command[i];
		if (quote == '\'') {
			if (c == '\'') quote = 0; else cur += c;
		} else if (quote == '"') {
			if (c == '"') quote = 0;
			else if (c == '\\' && i + 1 < command.size() &&
					std::string_view("\"\\$`").find(command[i + 1]) != std::string_view::npos)
				cur += command[++i];
			else cur += c;
		} else if (c == '\'' || c == '"') {
			quote = c;
			in_word = true;
		} else if (c == '\\' && i + 1 < command.size()) {
			cur += command[++i];
			in_word = true;
		} else if (c == ' ' || c == '\t' || c == '\n') {
			if (in_word) {
				words.push_back(cur);
				cur.clear();
				in_word = false;
			}
		} else {
			cur += c;
			in_word = true;
		}
	}
	if (in_word)
		words.push_back(cur);
	return words;
}

std::string join_command(const std::vector<std::string> &words) {
	std::string out;
	for (const std::string &w : words) {
		if (!out.empty())
			out += ' ';
		bool plain = !w.empty() && w.find_first_not_of(
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
				"-_./:=,+@%") == std::string::npos;
		if (plain) {
			out += w;
			continue;
		}
		out += '\'';
		for (char c : w) {
			if (c == '\'') out += "'\\''"; else out += c;
		}
		out += '\'';
	}
	return out;
}

std::map<std::string, std::string> read_options(const Integration &in,
		std::string_view command) {
	std::map<std::string, std::string> values;
	std::vector<std::string> words = split_command(command);
	for (const IntegrationOption &o : in.options)
		for (size_t i = 0; i + 1 < words.size(); ++i)
			if (words[i] == o.flag) {
				values[o.id] = words[i + 1];
				break;
			}
	return values;
}

std::string write_options(const Integration &in, std::string_view command,
		const std::map<std::string, std::string> &values) {
	std::vector<std::string> words = split_command(command);
	if (words.empty())
		words = split_command(in.exec);
	for (const IntegrationOption &o : in.options) {
		auto it = values.find(o.id);
		std::string value = it != values.end() ? it->second : "";
		size_t at = words.size();
		for (size_t i = 0; i + 1 < words.size(); ++i)
			if (words[i] == o.flag) {
				at = i;
				break;
			}
		if (at < words.size()) {
			if (value.empty())
				words.erase(words.begin() + at, words.begin() + at + 2);
			else
				words[at + 1] = value;
		} else if (!value.empty()) {
			words.push_back(o.flag);
			words.push_back(value);
		}
	}
	return join_command(words);
}

const Integration *match_integration(const std::vector<Integration> &all,
		std::string_view name, std::string_view command) {
	for (const Integration &in : all)
		if (in.id == name)
			return &in;
	std::string program = startup_name("", command);
	for (const Integration &in : all)
		if (!program.empty() && in.binary == program)
			return &in;
	return nullptr;
}

std::vector<std::string> programs_in_script(std::string_view script,
		const std::vector<std::string> &names) {
	std::set<std::string> found;
	std::istringstream lines{std::string(script)};
	std::string line;
	while (std::getline(lines, line)) {
		std::string_view l = trim(line);
		if (l.empty() || l[0] == '#')
			continue;
		/* Words, split on shell punctuation too: "(waybar || yambar) &". */
		std::string word;
		auto flush = [&] {
			if (!word.empty()) {
				std::string base = word.substr(word.rfind('/') == std::string::npos
						? 0 : word.rfind('/') + 1);
				if (std::find(names.begin(), names.end(), base) != names.end())
					found.insert(base);
			}
			word.clear();
		};
		for (char c : l) {
			if (c == '#' && word.empty())
				break;  /* a comment after code */
			if (std::string_view(" \t;&|()<>'\"`$").find(c) != std::string_view::npos)
				flush();
			else
				word += c;
		}
		flush();
	}
	return {found.begin(), found.end()};
}

}  // namespace wb
