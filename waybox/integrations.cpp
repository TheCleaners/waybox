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

namespace {

const std::set<std::string> kIntegrationKeys = {"Name", "Category", "Comment", "Exec", "Binary",
		"Version", "Configure", "Manages", "Page", "Kind", "Env", "Key", "FormatVersion",
		"Files", "Runs", "Reload"};

/* "a, b ,c" */
std::vector<std::string> comma_list(const std::string &value) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= value.size()) {
		size_t comma = value.find(',', start);
		std::string item(trim(std::string_view(value).substr(start,
				comma == std::string::npos ? std::string::npos : comma - start)));
		if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end())
			out.push_back(item);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}
const std::set<std::string> kOptionKeys = {"Label", "Flag", "Hint", "Type", "Default",
		"Choices", "Min", "Max"};
const std::set<std::string> kTypeNames = {"text", "file", "folder", "image", "image-folder",
		"image-or-folder", "color", "colour", "choice", "number", "switch"};

/* "2", "1.3": the major and minor; nullopt if not a version. */
std::optional<std::pair<int, int>> format_version(std::string_view v) {
	/* Not "major" and "minor": FreeBSD's <sys/types.h> has macros so named. */
	size_t dot = v.find('.');
	std::string hi(v.substr(0, dot)), lo = dot == std::string_view::npos ? "0"
			: std::string(v.substr(dot + 1));
	auto digits = [](const std::string &s) {
		return !s.empty() && s.size() <= 6 &&
				std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
	};
	if (!digits(hi) || !digits(lo))
		return std::nullopt;
	return std::make_pair(std::atoi(hi.c_str()), std::atoi(lo.c_str()));
}

}  // namespace

IntegrationCheck check_integration(std::string_view id, std::string_view text) {
	IntegrationCheck out;
	auto warn = [&out](int line, std::string msg) {
		out.problems.push_back({line, false, std::move(msg)});
	};
	auto fail = [&out](int line, std::string msg) {
		out.problems.push_back({line, true, std::move(msg)});
	};
	Integration in;
	in.id = std::string(id);
	for (unsigned char c : in.id)
		if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '+') {
			warn(0, "the file's name (its id) has \"" + std::string(1, static_cast<char>(c)) +
					"\": use letters, digits, '-', '_', '.', '+'");
			break;
		}
	std::string section;
	bool in_integration = false, seen_integration = false, newer = false;
	IntegrationOption *opt = nullptr;
	std::set<std::string> keys_here, option_ids;
	std::map<std::string, int> option_line;
	std::istringstream lines{std::string(text)};
	std::string raw;
	int n = 0;
	while (std::getline(lines, raw)) {
		++n;
		std::string_view line = trim(raw);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			section = std::string(trim(line.substr(1, line.size() - 2)));
			opt = nullptr;
			in_integration = section == "Integration";
			keys_here.clear();
			if (in_integration) {
				if (seen_integration)
					warn(n, "a second [Integration]: its keys go over the first's");
				seen_integration = true;
			} else if (section.rfind("Option ", 0) == 0) {
				std::string oid(trim(section.substr(7)));
				if (option_ids.count(oid))
					warn(n, "a second [Option " + oid + "]: only the first is used");
				option_ids.insert(oid);
				in.options.push_back({});
				in.options.back().id = oid;
				opt = &in.options.back();
				option_line[oid] = n;
			} else {
				warn(n, "unknown section [" + section + "]: ignored");
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string_view::npos) {
			warn(n, "not a Key=value line: ignored");
			continue;
		}
		std::string key(trim(line.substr(0, eq)));
		std::string value(trim(line.substr(eq + 1)));
		if (section.empty()) {
			warn(n, key + "= before any section: ignored");
			continue;
		}
		if (!in_integration && opt == nullptr)
			continue;  /* in an unknown section: said so once */
		const std::set<std::string> &known = in_integration ? kIntegrationKeys : kOptionKeys;
		if (!known.count(key)) {
			warn(n, "unknown key " + key + "= in [" + section + "]: ignored");
			continue;
		}
		if (keys_here.count(key) && key != "Env")
			warn(n, key + "= given again: this one is used");
		keys_here.insert(key);
		if (in_integration) {
			if (key == "Name") in.name = value;
			else if (key == "Category") in.category = value;
			else if (key == "Comment") in.comment = value;
			else if (key == "Exec") in.exec = value;
			else if (key == "Binary") in.binary = value;
			else if (key == "Version") in.version_command = value;
			else if (key == "Configure") in.configure = value;
			else if (key == "Manages") {
				in.manages_outputs = value == "outputs";
				if (value != "outputs")
					warn(n, "Manages=" + value + ": only \"outputs\" is known");
			}
			else if (key == "Page") {
				in.page = value;
				bool lua = value.size() > 4 && value.compare(value.size() - 4, 4, ".lua") == 0;
				if (lua && value.find('/') != std::string::npos)
					warn(n, "Page=" + value + ": a page's script is beside the file, named "
							"without a folder");
				else if (!lua && value != "wallpaper")
					warn(n, "Page=" + value + ": not a known page (wallpaper) or a .lua "
							"script; none shown");
			}
			else if (key == "Files") in.files = comma_list(value);
			else if (key == "Runs") in.runs = comma_list(value);
			else if (key == "Reload") {
				/* command PROGRAM ARGS, signal USR2, none */
				std::vector<std::string> words;
				std::istringstream in_words(value);
				for (std::string w; in_words >> w;)
					words.push_back(w);
				std::string sig = words.size() == 2 ? words[1] : "";
				if (sig.rfind("SIG", 0) == 0)
					sig = sig.substr(3);
				if (words.size() == 1 && words[0] == "none") {
					in.reload.kind = IntegrationReload::Kind::None;
				} else if (words.size() >= 2 && words[0] == "command") {
					in.reload.kind = IntegrationReload::Kind::Command;
					in.reload.argv.assign(words.begin() + 1, words.end());
				} else if (words.size() == 2 && words[0] == "signal" &&
						(sig == "USR1" || sig == "USR2" || sig == "HUP")) {
					in.reload.kind = IntegrationReload::Kind::Signal;
					in.reload.signal = sig;
				} else {
					warn(n, "Reload=" + value + ": command PROGRAM ARGS, signal USR1, USR2 "
							"or HUP, or none; ignored");
				}
			}
			else if (key == "Kind") {
				in.binding = value == "binding";
				in.input_method = value == "inputmethod";
				if (!in.binding && !in.input_method && value != "startup")
					warn(n, "Kind=" + value + ": not binding, inputmethod or startup; "
							"started with the session");
			} else if (key == "Env") {
				size_t e = value.find('=');
				if (e != std::string::npos && e > 0)
					in.env.emplace_back(value.substr(0, e), value.substr(e + 1));
				else
					warn(n, "Env=" + value + ": not NAME=VALUE; ignored");
			}
			else if (key == "Key") in.key = value;
			else if (key == "FormatVersion") {
				auto v = format_version(value);
				if (!v) {
					warn(n, "FormatVersion=" + value + ": not a version (1, 1.2); read as 1");
				} else if (v->first > kIntegrationFormat) {
					fail(n, "FormatVersion=" + value + " is newer than this version of "
							"Waybox reads (" + std::to_string(kIntegrationFormat) +
							"): the file is skipped");
					newer = true;
				}
			}
		} else {
			if (key == "Label") opt->label = value;
			else if (key == "Flag") opt->flag = value;
			else if (key == "Hint") opt->hint = value;
			else if (key == "Type") {
				opt->type = option_type_from_name(value);
				if (!kTypeNames.count(value))
					warn(n, "Type=" + value + ": not a known type; text");
			}
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
				if (auto num = parse_number(value))
					(key == "Min" ? opt->min : opt->max) = num;
				else
					warn(n, key + "=" + value + ": not a number; ignored");
			}
		}
	}
	if (!seen_integration)
		fail(0, "no [Integration] section");
	else if (in.name.empty())
		fail(0, "no Name= in [Integration]");
	/* A command: some word that isn't empty ("" '' isn't one). */
	std::vector<std::string> words = split_command(in.exec);
	bool command = std::any_of(words.begin(), words.end(),
			[](const std::string &w) { return !w.empty(); });
	if (seen_integration && in.exec.empty())
		fail(0, "no Exec= in [Integration]");
	else if (!in.exec.empty() && !command)
		fail(0, "Exec= has no command");
	if (in.binding && in.key.empty())
		warn(0, "Kind=binding without a Key=: none suggested");
	bool lua_page = in.page.size() > 4 && in.page.compare(in.page.size() - 4, 4, ".lua") == 0;
	if (!lua_page && (!in.files.empty() || !in.runs.empty()))
		warn(0, "Files= and Runs= are for a Lua page (Page=NAME.lua): ignored");
	for (const std::string &r : in.runs) {
		if (r.find('/') != std::string::npos || r.find(' ') != std::string::npos)
			warn(0, "Runs=" + r + ": a program's name, without a folder or arguments");
		else if (std::string why = full_access_run(r); !why.empty())
			warn(0, "Runs=" + r + " is full access to the account (" + why + "): a page "
					"shouldn't need it");
	}
	for (const std::string &f : in.files)
		if (std::string why = full_access_file(f); !why.empty())
			warn(0, "Files=" + f + " is full access to the account (" + why + "): a page "
					"shouldn't need it");
	if (in.reload.kind == IntegrationReload::Kind::Command) {
		const std::string &r = in.reload.argv[0];
		if (r.find('/') != std::string::npos)
			warn(0, "Reload=command " + r + ": a program's name, without a folder");
		else if (std::string why = full_access_run(r); !why.empty())
			warn(0, "Reload=command " + r + " is full access to the account (" + why + ")");
	}
	if (in.reload.kind == IntegrationReload::Kind::Signal &&
			(in.binary.empty() ? startup_name("", in.exec) : in.binary) == "waybox")
		fail(0, "Reload=signal: not to Waybox itself");
	/* Options: what's dropped or guessed, said where it's written. */
	std::set<std::string> first_seen;
	std::vector<IntegrationOption> kept;
	for (IntegrationOption &o : in.options) {
		int at = option_line[o.id];
		if (o.id.empty()) {
			warn(at, "[Option] without an id: ignored");
			continue;
		}
		if (!first_seen.insert(o.id).second)
			continue;  /* the duplicate, said above */
		if (o.flag.empty()) {
			warn(at, "[Option " + o.id + "] has no Flag=: ignored (wbconf finds and "
					"writes an option by its flag)");
			continue;
		}
		if (o.label.empty())
			o.label = o.id;
		if (o.type == OptionType::Choice && o.choices.empty()) {
			warn(at, "[Option " + o.id + "] Type=choice without Choices=: text");
			o.type = OptionType::Text;
		}
		if (!o.choices.empty() && o.type != OptionType::Choice)
			warn(at, "[Option " + o.id + "] has Choices= but isn't Type=choice: they're "
					"not offered");
		if ((o.min || o.max) && o.type != OptionType::Number)
			warn(at, "[Option " + o.id + "] has Min=/Max= but isn't Type=number: not checked");
		if (o.min && o.max && *o.min > *o.max) {
			warn(at, "[Option " + o.id + "] Min= is above Max=: neither is used");
			o.min = o.max = std::nullopt;
		}
		if (o.type == OptionType::Switch && !o.default_value.empty() &&
				o.default_value != "on" && o.default_value != "off")
			warn(at, "[Option " + o.id + "] is a switch: Default= is \"on\" or \"off\"");
		kept.push_back(std::move(o));
	}
	in.options = std::move(kept);
	std::stable_sort(out.problems.begin(), out.problems.end(),
			[](const IntegrationProblem &a, const IntegrationProblem &b) {
				return a.line < b.line;
			});
	bool usable = !newer && !in.name.empty() && command;
	if (!usable)
		return out;
	if (in.binary.empty())
		in.binary = startup_name("", in.exec);
	if (in.category.empty())
		in.category = "Other";
	out.integration = std::move(in);
	return out;
}

std::optional<Integration> parse_integration(std::string_view id, std::string_view text) {
	return check_integration(id, text).integration;
}

std::string full_access_run(const std::string &program) {
	/* python3.13, perl5.40, lua5.4: as python, perl, lua. */
	std::string name = program;
	while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back())) ||
			name.back() == '.' || name.back() == '-'))
		name.pop_back();
	static const std::set<std::string> any = {
		/* shells */
		"sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "oksh", "tcsh", "csh", "yash",
		"busybox", "toybox", "elvish", "nu", "xonsh",
		/* interpreters */
		"python", "pypy", "perl", "ruby", "irb", "lua", "luajit", "node", "nodejs", "deno",
		"bun", "php", "tclsh", "wish", "guile", "racket", "R", "Rscript", "julia", "osascript",
		"awk", "gawk", "mawk", "nawk", "busybox-awk",
		/* running another command */
		"env", "xargs", "nohup", "setsid", "timeout", "nice", "ionice", "stdbuf", "chrt",
		"taskset", "systemd-run", "flatpak-spawn", "flatpak", "watch", "script", "expect",
		"parallel", "unshare", "nsenter", "chroot", "dbus-launch", "uwsm", "exec",
		/* privilege */
		"sudo", "doas", "pkexec", "su", "runuser", "run0",
		/* tools that can run commands themselves */
		"find", "sed", "make", "git", "ssh", "gdb", "vi", "vim", "nvim", "emacs", "nano",
		"ed", "ex", "less", "more", "man", "tar", "rsync", "crontab", "at", "batch",
		"systemctl", "waybox-msg",
		/* terminals: they run the command they're given */
		"foot", "footclient", "kitty", "alacritty", "wezterm", "xterm", "uxterm", "st",
		"urxvt", "rxvt", "gnome-terminal", "kgx", "ptyxis", "konsole", "tilix", "terminator",
		"xfce4-terminal", "lxterminal", "qterminal", "sakura", "termite", "x-terminal-emulator",
		/* D-Bus: asking other programs (systemd, the session) to start things */
		"dbus-send", "gdbus", "busctl", "qdbus", "qdbus6", "dbus-update-activation-environment",
		/* launchers and compositors' own commands */
		"xdg-open", "gtk-launch", "gio", "kioclient", "kde-open", "swaymsg", "hyprctl",
		"niri", "labwc", "riverctl",
		/* typing keys: into a terminal, as anything */
		"wtype", "ydotool", "xdotool", "kdotool", "dotool", "wlrctl",
	};
	return any.count(name) ? "it can run any command" : "";
}

std::string full_access_file(const std::string &path, const std::string &home) {
	std::string p = path;
	if (!home.empty() && p.compare(0, home.size(), home) == 0 &&
			(p.size() == home.size() || p[home.size()] == '/'))
		p = "~" + p.substr(home.size());
	if (p.rfind("~/", 0) != 0)
		return "";
	std::string rel = p.substr(2);
	static const std::set<std::string> startup = {
		".bashrc", ".bash_profile", ".bash_login", ".bash_logout", ".profile", ".zshrc",
		".zprofile", ".zshenv", ".zlogin", ".kshrc", ".cshrc", ".tcshrc", ".mkshrc",
		".xinitrc", ".xprofile", ".xsession", ".xsessionrc", ".Xresources",
		".pam_environment", ".inputrc", ".gitconfig",
	};
	if (startup.count(rel))
		return "it runs at your next login";
	static const std::vector<std::pair<std::string, const char *>> dirs = {
		{".config/waybox/", "Waybox runs what it says"},
		{".config/autostart/", "it runs at your next login"},
		{".config/environment.d/", "it runs at your next login"},
		{".config/fish/", "it runs at your next login"},
		{".config/systemd/", "it runs at your next login"},
		{".config/labwc/", "it runs at your next login"},
		{".local/bin/", "it's run in place of a program"},
		{"bin/", "it's run in place of a program"},
		{".ssh/", "it opens access to your account"},
		{".gnupg/", "it opens access to your account"},
		{".config/git/", "it runs commands when you use git"},
	};
	for (const auto &[dir, why] : dirs)
		if (rel.compare(0, dir.size(), dir) == 0)
			return why;
	return "";
}

namespace {

/* "~/.config/mako/config" for mako: in its own folder in one of the
 * places programs keep theirs (not any folder of that name: a page
 * naming its program "share" doesn't own ~/.local/share). */
bool own_file(const std::string &tilde_path, const std::string &binary) {
	if (binary.empty() || binary.find('/') != std::string::npos || binary[0] == '.')
		return false;
	for (const char *base : {"~/.config/", "~/.local/share/", "~/.local/state/", "~/.cache/",
			"~/.", "/etc/xdg/"}) {
		std::string dir = std::string(base) + binary + "/";
		if (tilde_path.size() > dir.size() && tilde_path.compare(0, dir.size(), dir) == 0 &&
				tilde_path.find("/../") == std::string::npos)
			return true;
	}
	return false;
}

}  // namespace

PageAccess run_access(const Integration &in, const std::string &program) {
	if (std::string why = full_access_run(program); !why.empty())
		return {Reach::Full, why};
	/* Its program, or a companion named after it: makoctl, swaymsg,
	 * footclient. Exactly so: the page names its program itself, and a
	 * prefix would let "s" cover strace or steam. */
	const std::string &b = in.binary;
	if (!b.empty() && program.compare(0, b.size(), b) == 0) {
		static const std::set<std::string> companion = {"", "ctl", "-ctl", "msg", "-msg",
				"client", "-client", "cli", "-cli"};
		if (companion.count(program.substr(b.size())))
			return {Reach::Own, ""};
	}
	return {Reach::Other, ""};
}

PageAccess file_access(const Integration &in, const std::string &path,
		const std::string &home) {
	if (std::string why = full_access_file(path, home); !why.empty())
		return {Reach::Full, why};
	std::string p = path;
	if (!home.empty() && p.compare(0, home.size(), home) == 0 &&
			(p.size() == home.size() || p[home.size()] == '/'))
		p = "~" + p.substr(home.size());
	if (own_file(p, in.binary))
		return {Reach::Own, ""};
	return {Reach::Other, ""};
}

std::string lua_page_path(const Integration &in) {
	const std::string &p = in.page;
	if (p.size() <= 4 || p.compare(p.size() - 4, 4, ".lua") != 0 || p.find('/') != std::string::npos
			|| in.dir.empty())
		return "";
	return in.dir + "/" + p;
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
				parsed->dir = dir;
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
