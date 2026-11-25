/*
 * Integrations (see integrations.hpp).
 */
#include "waybox/integrations.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
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

OptionType option_type_from_name(std::string_view n) {
	if (n == "file") return OptionType::File;
	if (n == "folder") return OptionType::Folder;
	if (n == "image") return OptionType::Image;
	if (n == "image-folder") return OptionType::ImageFolder;
	if (n == "image-or-folder") return OptionType::ImageOrFolder;
	if (n == "color" || n == "colour") return OptionType::Color;
	if (n == "choice") return OptionType::Choice;
	if (n == "number") return OptionType::Number;
	if (n == "switch") return OptionType::Switch;
	return OptionType::Text;
}

namespace {

/* A whole decimal number ("12", "-0.5"), else nullopt. */
std::optional<double> parse_number(std::string_view s) {
	if (s.empty() || s.size() > 64)
		return std::nullopt;
	std::string str(s);
	char *end = nullptr;
	errno = 0;
	double d = strtod(str.c_str(), &end);
	if (end != str.c_str() + str.size() || errno != 0 || d != d ||
			str.find_first_of("xXnNiI") != std::string::npos)  /* hex, nan, inf */
		return std::nullopt;
	return d;
}

}  // namespace

bool option_value_valid(const IntegrationOption &o, std::string_view value) {
	if (value.empty())
		return true;
	switch (o.type) {
	case OptionType::Number: {
		auto n = parse_number(value);
		return n && (!o.min || *n >= *o.min) && (!o.max || *n <= *o.max);
	}
	case OptionType::Choice:
		return std::find(o.choices.begin(), o.choices.end(), value) != o.choices.end();
	case OptionType::Switch:
		return value == "on";
	default:
		return true;
	}
}

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
				in.options.push_back({});
				in.options.back().id = std::string(trim(section.substr(7)));
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
			else if (key == "Version") in.version_command = value;
			else if (key == "Configure") in.configure = value;
			else if (key == "Page") in.page = value;
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
			else if (key == "Type") opt->type = option_type_from_name(value);
			else if (key == "Default") opt->default_value = value;
			else if (key == "Choices") {
				opt->choices.clear();
				size_t start = 0;
				while (start <= value.size()) {
					size_t comma = value.find(',', start);
					std::string c(trim(std::string_view(value).substr(start,
							comma == std::string::npos ? std::string::npos : comma - start)));
					if (!c.empty() && std::find(opt->choices.begin(), opt->choices.end(), c) ==
							opt->choices.end())
						opt->choices.push_back(c);
					if (comma == std::string::npos)
						break;
					start = comma + 1;
				}
			} else if (key == "Min" || key == "Max") {
				if (auto n = parse_number(value))
					(key == "Min" ? opt->min : opt->max) = n;
			}
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
	for (IntegrationOption &o : in.options) {
		if (o.label.empty())
			o.label = o.id;
		/* A choice with nothing to choose is just text. */
		if (o.type == OptionType::Choice && o.choices.empty())
			o.type = OptionType::Text;
		if (o.min && o.max && *o.min > *o.max)
			o.min = o.max = std::nullopt;
	}
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
	for (const IntegrationOption &o : in.options) {
		bool sw = o.type == OptionType::Switch;
		for (size_t i = 0; i + (sw ? 0 : 1) < words.size(); ++i)
			if (words[i] == o.flag) {
				values[o.id] = sw ? "on" : words[i + 1];
				break;
			}
	}
	return values;
}

namespace {

/* A shell word of a command and where it is in the text. */
struct Word {
	std::string text;
	size_t start, end;  /* [start, end) in the command */
};

std::vector<Word> words_with_spans(std::string_view command) {
	std::vector<Word> out;
	size_t i = 0;
	while (i < command.size()) {
		while (i < command.size() && (command[i] == ' ' || command[i] == '\t' || command[i] == '\n'))
			++i;
		if (i >= command.size())
			break;
		size_t start = i;
		char quote = 0;
		while (i < command.size()) {
			char c = command[i];
			if (quote) {
				if (c == quote)
					quote = 0;
				else if (c == '\\' && quote == '"' && i + 1 < command.size())
					++i;
			} else if (c == '\'' || c == '"') {
				quote = c;
			} else if (c == '\\' && i + 1 < command.size()) {
				++i;
			} else if (c == ' ' || c == '\t' || c == '\n') {
				break;
			}
			++i;
		}
		std::string_view raw = command.substr(start, i - start);
		auto parsed = split_command(raw);
		out.push_back({parsed.empty() ? std::string() : parsed[0], start, i});
	}
	return out;
}

/* An option's value as a shell word: bare when it's plain (so ~ and $VAR
 * still expand, as typed), else single-quoted. */
std::string value_word(const std::string &v) {
	bool plain = !v.empty() && v.find_first_not_of(
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
			"-_./:=,+@%~${}") == std::string::npos;
	return plain ? v : join_command({v});
}

}  // namespace

std::string write_options(const Integration &in, std::string_view command,
		const std::map<std::string, std::string> &values) {
	/* Only the options' values change; the rest of the command stays as
	 * it was written (quoting, ~, $VAR, pipes...). */
	std::string out(command.empty() ? std::string_view(in.exec) : command);
	for (const IntegrationOption &o : in.options) {
		auto it = values.find(o.id);
		std::string value = it != values.end() ? it->second : "";
		std::vector<Word> words = words_with_spans(out);
		size_t at = words.size();
		if (o.type == OptionType::Switch) {
			for (size_t i = 0; i < words.size(); ++i)
				if (words[i].text == o.flag) {
					at = i;
					break;
				}
			if (at < words.size() && value.empty()) {
				size_t from = words[at].start;
				while (from > 0 && (out[from - 1] == ' ' || out[from - 1] == '\t'))
					--from;
				out.erase(from, words[at].end - from);
			} else if (at == words.size() && !value.empty()) {
				out += " " + o.flag;
			}
			continue;
		}
		for (size_t i = 0; i + 1 < words.size(); ++i)
			if (words[i].text == o.flag) {
				at = i;
				break;
			}
		if (at < words.size()) {
			if (value.empty()) {
				/* The flag and its value go, with the space before them. */
				size_t from = words[at].start;
				while (from > 0 && (out[from - 1] == ' ' || out[from - 1] == '\t'))
					--from;
				out.erase(from, words[at + 1].end - from);
			} else if (words[at + 1].text != value) {
				out.replace(words[at + 1].start, words[at + 1].end - words[at + 1].start,
						value_word(value));
			}
		} else if (!value.empty()) {
			out += " " + o.flag + " " + value_word(value);
		}
	}
	return out;
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

std::string version_from_output(std::string_view output) {
	size_t i = 0;
	while (i < output.size()) {
		/* A word: up to whitespace, or punctuation that ends a version. */
		while (i < output.size() && std::string_view(" \t\r\n(),;:\"'").find(output[i]) !=
				std::string_view::npos)
			++i;
		size_t start = i;
		while (i < output.size() && std::string_view(" \t\r\n(),;:\"'").find(output[i]) ==
				std::string_view::npos)
			++i;
		std::string_view w = output.substr(start, i - start);
		if (!w.empty() && (w[0] == 'v' || w[0] == 'V'))
			w.remove_prefix(1);
		/* digits, a dot, a digit: 1.2, 0.15.0, 14.0.0-rc1 */
		size_t d = 0;
		while (d < w.size() && std::isdigit(static_cast<unsigned char>(w[d])))
			++d;
		if (d > 0 && d + 1 < w.size() && w[d] == '.' &&
				std::isdigit(static_cast<unsigned char>(w[d + 1]))) {
			while (!w.empty() && (w.back() == '.' || w.back() == '-'))
				w.remove_suffix(1);
			return std::string(w);
		}
	}
	return "";
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

ProgramStatus program_status(bool installed, bool running, bool in_script,
		bool switched_on) {
	ProgramStatus st;
	st.not_installed = !installed;
	st.running = installed && running;
	st.twice = in_script && switched_on;
	st.from_script = in_script && !switched_on;
	return st;
}

}  // namespace wb
