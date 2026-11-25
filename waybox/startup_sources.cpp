#include "waybox/startup_sources.hpp"

#include <algorithm>
#include <set>
#include <map>
#include <sstream>

#include "waybox/startup.hpp"

namespace wb {

namespace {

std::vector<std::string> split_lines(std::string_view text) {
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		if (nl == std::string_view::npos) {
			if (start < text.size())
				lines.emplace_back(text.substr(start));
			break;
		}
		lines.emplace_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	return lines;
}

std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

/* Shell words, quotes kept inside a word; false on an unterminated quote.
 * Unquoted operators (; & | && || < > and redirections) come out as words
 * of their own, so the caller can tell them from arguments. A comment
 * ends the line. */
bool shell_words(std::string_view s, std::vector<std::string> &out) {
	std::string w;
	bool in_word = false;
	auto flush = [&] {
		if (in_word)
			out.push_back(w);
		w.clear();
		in_word = false;
	};
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '\'' || c == '"') {
			size_t end = s.find(c, i + 1);
			if (end == std::string_view::npos)
				return false;
			w += s.substr(i, end - i + 1);
			in_word = true;
			i = end;
		} else if (c == ' ' || c == '\t') {
			flush();
		} else if (c == '#' && !in_word) {
			break;
		} else if (c == '&' && in_word && (w.back() == '>' || w.back() == '<')) {
			w += c;  /* 2>&1: a redirection, not "run in the background" */
		} else if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')' ||
				c == '`') {
			flush();
			std::string op(1, c);
			if ((c == '&' || c == '|') && i + 1 < s.size() && s[i + 1] == c)
				op += s[++i];
			out.push_back(op);
		} else {
			w += c;
			in_word = true;
		}
	}
	flush();
	return true;
}

bool is_redirection(const std::string &w) {
	/* >file, >>file, 2>&1, 2>/dev/null, </dev/null */
	size_t i = 0;
	while (i < w.size() && std::isdigit(static_cast<unsigned char>(w[i])))
		++i;
	return i < w.size() && (w[i] == '>' || w[i] == '<');
}

const char *const kOpeners[] = {"if", "case", "for", "while", "until", "{"};
const char *const kClosers[] = {"fi", "esac", "done", "}"};

/* How a line changes the nesting of if/case/loops/braces. */
int nesting_change(const std::vector<std::string> &words) {
	int d = 0;
	bool command_start = true;
	for (const std::string &w : words) {
		if (command_start) {
			for (const char *o : kOpeners)
				if (w == o)
					++d;
			for (const char *c : kClosers)
				if (w == c)
					--d;
		}
		command_start = w == ";" || w == "&&" || w == "||" || w == "|" ||
				w == "&" || w == "then" || w == "do" || w == "else" || w == "(";
	}
	/* name() { ... : a function body opening at the line's end. */
	if (!words.empty() && words.back() == "{" && d == 0)
		++d;
	return d;
}

std::string basename_of(const std::string &w) {
	size_t slash = w.rfind('/');
	return slash == std::string::npos ? w : w.substr(slash + 1);
}

/* The program a simple start line runs ("" if it isn't one). */
std::string simple_start(const std::vector<std::string> &words) {
	if (words.size() < 2 || words.back() != "&")
		return "";
	size_t i = 0;
	std::string guarded;
	/* command -v prog [redirections] && / which prog [redirections] && */
	if (words[0] == "command" && words.size() > 3 && words[1] == "-v") {
		guarded = words[2];
		i = 3;
	} else if (words[0] == "which" && words.size() > 2) {
		guarded = words[1];
		i = 2;
	}
	if (!guarded.empty()) {
		while (i < words.size() && is_redirection(words[i]))
			++i;
		if (i >= words.size() || words[i] != "&&")
			return "";
		++i;
	}
	if (i >= words.size() - 1)
		return "";
	const std::string &run = words[i];
	if (run.find('=') != std::string::npos || run[0] == '\'' || run[0] == '"' ||
			run[0] == '$' || is_redirection(run))
		return "";
	for (const char *k : kOpeners)
		if (run == k)
			return "";
	for (const char *k : kClosers)
		if (run == k)
			return "";
	if (!guarded.empty() && basename_of(guarded) != basename_of(run))
		return "";
	/* Arguments and redirections only, up to the final &. */
	for (size_t j = i + 1; j + 1 < words.size(); ++j) {
		const std::string &w = words[j];
		if (w == ";" || w == "&" || w == "&&" || w == "||" || w == "|" ||
				w == "(" || w == ")" || w == "`")
			return "";
	}
	return basename_of(run);
}

/* The line without its comment mark, if it is commented ("#prog &",
 * "# prog &"); nullopt-like: empty `rest` means not commented. */
bool uncomment(std::string_view line, std::string &rest) {
	std::string_view t = trim(line);
	if (t.empty() || t[0] != '#')
		return false;
	t.remove_prefix(1);
	if (!t.empty() && t[0] == '!')
		return false;  /* a shebang */
	rest = std::string(trim(t));
	return true;
}

/* What a line of a script is, so only lines that are commands of their own
 * are ever offered to be switched:
 *   Code       a line of its own (or a comment)
 *   Continued  part of a command spanning lines: it ends with "\", or the
 *              line before it did
 *   Data       inside a here-document (<<WORD ... WORD, its end line
 *              included) or a quoted string that spans lines */
enum class LineKind { Code, Continued, Data };

std::vector<LineKind> classify_lines(const std::vector<std::string> &lines) {
	std::vector<LineKind> kinds(lines.size(), LineKind::Code);
	struct Heredoc { std::string word; bool strip_tabs; };
	std::vector<Heredoc> pending;  /* here-docs started, in order */
	char open_quote = 0;           /* a quote left open by an earlier line */
	bool continued = false;        /* the line before ended with "\" */
	for (size_t n = 0; n < lines.size(); ++n) {
		const std::string &l = lines[n];
		if (!pending.empty()) {
			kinds[n] = LineKind::Data;
			std::string_view body = l;
			if (pending.front().strip_tabs)
				while (!body.empty() && body.front() == '\t')
					body.remove_prefix(1);
			if (!body.empty() && body.back() == '\r')
				body.remove_suffix(1);
			if (body == pending.front().word)
				pending.erase(pending.begin());
			continue;
		}
		size_t i = 0;
		if (open_quote != 0) {
			kinds[n] = LineKind::Data;
			/* Up to the closing quote; the rest of the line is scanned on. */
			for (; i < l.size(); ++i) {
				if (l[i] == '\\' && open_quote == '"') {
					++i;
				} else if (l[i] == open_quote) {
					open_quote = 0;
					++i;
					break;
				}
			}
			if (open_quote != 0)
				continue;
		} else if (continued) {
			kinds[n] = LineKind::Continued;
		} else {
			std::string_view t = trim(l);
			if (!t.empty() && t.front() == '#')
				continue;  /* a comment: nothing in it counts */
		}
		/* The rest: quotes, comments, here-doc starts, a final backslash. */
		bool word_start = true, escaped_end = false;
		char quote = 0;
		for (; i < l.size(); ++i) {
			char c = l[i];
			escaped_end = false;
			if (quote != 0) {
				if (c == '\\' && quote == '"' && i + 1 < l.size())
					++i;
				else if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '\\') {
				if (i + 1 == l.size())
					escaped_end = true;
				else
					++i;
				word_start = false;
				continue;
			}
			if (c == '\'' || c == '"') {
				quote = c;
				word_start = false;
				continue;
			}
			if (c == '#' && word_start)
				break;
			if (c == '<' && i + 1 < l.size() && l[i + 1] == '<' &&
					(i + 2 >= l.size() || l[i + 2] != '<')) {
				i += 2;
				bool strip = i < l.size() && l[i] == '-';
				if (strip)
					++i;
				while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
					++i;
				std::string word;
				for (; i < l.size(); ++i) {
					char w = l[i];
					if (w == '\'' || w == '"' || w == '\\')
						continue;  /* <<'EOF', <<"EOF", <<\EOF: the word unquoted */
					if (w == ' ' || w == '\t' || w == ';' || w == '&' || w == '|' ||
							w == '>' || w == '<' || w == ')')
						break;
					word += w;
				}
				if (!word.empty())
					pending.push_back({word, strip});
				--i;
				word_start = false;
				continue;
			}
			word_start = c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' ||
					c == '(' || c == ')';
		}
		if (quote != 0) {
			open_quote = quote;
			kinds[n] = LineKind::Data;  /* opens a string: not a line of its own */
		}
		/* "\" at the end continues the command on the next line. */
		bool ends_continued = escaped_end && quote == 0;
		if (ends_continued && kinds[n] == LineKind::Code)
			kinds[n] = LineKind::Continued;
		continued = ends_continued;
	}
	return kinds;
}

/* "name() {" or "function name": a shell function's name, else "". */
std::string function_defined(std::string_view line) {
	std::string_view l = trim(line);
	if (l.substr(0, 9) == "function ")
		l = trim(l.substr(9));
	size_t n = 0;
	while (n < l.size() && (std::isalnum(static_cast<unsigned char>(l[n])) ||
			l[n] == '_' || l[n] == '-'))
		++n;
	if (n == 0)
		return "";
	std::string_view rest = trim(l.substr(n));
	if (rest.substr(0, 2) == "()" || line.find("function ") != std::string_view::npos)
		return std::string(l.substr(0, n));
	return "";
}

}  // namespace

std::vector<ScriptEntry> script_entries(std::string_view script) {
	std::vector<ScriptEntry> out;
	std::vector<std::string> lines = split_lines(script);
	std::vector<LineKind> kinds = classify_lines(lines);
	std::vector<std::string> functions;
	for (size_t n = 0; n < lines.size(); ++n)
		if (kinds[n] == LineKind::Code)
			if (std::string f = function_defined(lines[n]); !f.empty())
				functions.push_back(f);
	int depth = 0;
	for (size_t n = 0; n < lines.size(); ++n) {
		if (kinds[n] == LineKind::Data)
			continue;
		std::string commented;
		bool is_comment = uncomment(lines[n], commented);
		std::vector<std::string> words;
		if (!shell_words(is_comment ? std::string_view(commented)
				: std::string_view(lines[n]), words))
			continue;
		if (depth == 0 && kinds[n] == LineKind::Code) {
			std::string prog = simple_start(words);
			if (!prog.empty()) {
				ScriptEntry e;
				e.program = prog;
				e.command = std::string(is_comment ? std::string_view(commented)
						: trim(lines[n]));
				e.line = static_cast<int>(n);
				e.enabled = !is_comment;
				e.function = std::find(functions.begin(), functions.end(), prog) !=
						functions.end();
				out.push_back(std::move(e));
			}
		}
		if (!is_comment)
			depth = std::max(0, depth + nesting_change(words));
	}
	return out;
}

namespace {

/* Whether `line` (code, not a comment) may run `program`: it's a word of
 * its own, not in quotes ("swaybg started"), not a redirection, and not
 * what a check or a kill names (which, command -v, pkill, ...). */
bool mentions(std::string_view line, const std::string &program) {
	std::vector<std::string> words;
	if (!shell_words(line, words))
		return false;
	static const char *const kNames[] = {"which", "type", "hash", "pkill", "killall",
			"pgrep", "pidof", "kill"};
	bool skip_next = false;
	for (size_t i = 0; i < words.size(); ++i) {
		const std::string &w = words[i];
		if (skip_next) {
			skip_next = false;
			if (!w.empty() && w[0] != '-')
				continue;
			skip_next = true;  /* past its options */
			continue;
		}
		/* (A quoted word keeps its quotes, so "swaybg started" never
		 * equals a program's name.) */
		if (w.empty() || is_redirection(w))
			continue;
		size_t slash = w.rfind('/');
		std::string base = w.substr(slash == std::string::npos ? 0 : slash + 1);
		if (std::find(std::begin(kNames), std::end(kNames), base) != std::end(kNames)) {
			skip_next = true;
			continue;
		}
		if (base == "command" && i + 1 < words.size() &&
				(words[i + 1] == "-v" || words[i + 1] == "-V")) {
			++i;
			skip_next = true;
			continue;
		}
		if (base == program)
			return true;
	}
	return false;
}

}  // namespace

std::string script_start_command(std::string_view line) {
	std::string_view l = trim(line);
	if (!l.empty() && l.back() == '&' && (l.size() < 2 || l[l.size() - 2] != '&'))
		l = trim(l.substr(0, l.size() - 1));
	/* A guard: what follows the last "&&" outside quotes. */
	size_t cut = std::string_view::npos;
	char quote = 0;
	for (size_t i = 0; i + 1 < l.size(); ++i) {
		char c = l[i];
		if (quote != 0) {
			if (c == quote)
				quote = 0;
			else if (c == '\\' && quote == '"')
				++i;
			continue;
		}
		if (c == '\'' || c == '"')
			quote = c;
		else if (c == '\\')
			++i;
		else if (c == '&' && l[i + 1] == '&')
			cut = i;
	}
	if (cut != std::string_view::npos)
		l = trim(l.substr(cut + 2));
	return std::string(l);
}

ScriptStarters script_starters(std::string_view script, const std::string &program) {
	ScriptStarters out;
	std::vector<std::string> lines = split_lines(script);
	std::vector<LineKind> kinds = classify_lines(lines);
	auto data = [&kinds](size_t n) { return kinds[n] == LineKind::Data; };
	/* Function bodies: from "name() {" until its braces close. */
	std::map<std::string, bool> runs;  /* function -> runs the program */
	std::vector<bool> in_body(lines.size(), false);
	for (size_t n = 0; n < lines.size(); ++n) {
		if (kinds[n] != LineKind::Code)
			continue;
		std::string name = function_defined(lines[n]);
		if (name.empty())
			continue;
		int depth = 0;
		bool opened = false, found = false;
		size_t m = n;
		for (; m < lines.size(); ++m) {
			std::string commented;
			if (data(m) || uncomment(lines[m], commented)) {
				in_body[m] = true;
				continue;
			}
			in_body[m] = true;
			if (m > n && mentions(lines[m], program))
				found = true;
			std::vector<std::string> words;
			if (shell_words(lines[m], words))
				depth += nesting_change(words);
			if (depth > 0)
				opened = true;
			if (opened && depth <= 0)
				break;
		}
		runs[name] = found;
		n = m;
	}
	/* A function that runs it, called from another function that's
	 * called: one level of calls is enough for an autostart script. */
	std::set<int> entry_lines;
	for (const ScriptEntry &e : script_entries(script)) {
		if (!e.enabled)
			continue;
		bool direct = !e.function && e.program == program;
		bool via = e.function && runs.count(e.program) && runs[e.program];
		if (direct || via) {
			out.lines.push_back(e);
			entry_lines.insert(e.line);
			if (via)
				out.functions.push_back(e.program);
		}
	}
	for (size_t n = 0; n < lines.size(); ++n) {
		std::string commented;
		if (in_body[n] || data(n) || entry_lines.count(static_cast<int>(n)) ||
				uncomment(lines[n], commented))
			continue;
		if (mentions(lines[n], program))
			out.elsewhere = true;
	}
	/* Run by a function, but no line we can switch calls it. */
	for (const auto &[name, yes] : runs)
		if (yes && std::find(out.functions.begin(), out.functions.end(), name) ==
				out.functions.end()) {
			bool called = false;
			for (size_t n = 0; n < lines.size(); ++n) {
				std::string commented;
				if (!in_body[n] && !data(n) && !uncomment(lines[n], commented) &&
						mentions(lines[n], name))
					called = true;
			}
			if (called)
				out.elsewhere = true;
		}
	return out;
}

std::string script_set_enabled(std::string_view script, int line, bool on) {
	std::vector<ScriptEntry> entries = script_entries(script);
	auto it = std::find_if(entries.begin(), entries.end(),
			[line](const ScriptEntry &e) { return e.line == line; });
	if (it == entries.end() || it->enabled == on)
		return std::string(script);
	/* Rebuild with only that line changed. */
	std::string out;
	size_t start = 0;
	int n = 0;
	while (start <= script.size()) {
		size_t nl = script.find('\n', start);
		std::string_view l = script.substr(start,
				nl == std::string_view::npos ? std::string_view::npos : nl - start);
		if (n == line) {
			size_t indent = l.find_first_not_of(" \t");
			if (indent == std::string_view::npos)
				indent = l.size();
			if (on) {
				std::string_view body = l.substr(indent + 1);  /* past '#' */
				if (!body.empty() && body[0] == ' ')
					body.remove_prefix(1);
				out += std::string(l.substr(0, indent)) + std::string(body);
			} else {
				out += std::string(l.substr(0, indent)) + "#" + std::string(l.substr(indent));
			}
		} else {
			out += l;
		}
		if (nl == std::string_view::npos)
			break;
		out += '\n';
		start = nl + 1;
		++n;
	}
	return out;
}

namespace {

/* Remove Exec field codes (%f %F %u %U %i %c %k ...; %% is a percent). */
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

std::vector<std::string> split_list(std::string_view v) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : v) {
		if (c == ';') {
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

bool is_true(std::string_view v) { return trim(v) == "true"; }

}  // namespace

bool parse_autostart(std::string_view text, const std::vector<std::string> &desktops,
		AutostartEntry &e) {
	bool in_entry = false, seen_entry = false;
	std::vector<std::string> only, not_in;
	for (const std::string &raw : split_lines(text)) {
		std::string_view l = trim(raw);
		if (l.empty() || l[0] == '#')
			continue;
		if (l[0] == '[') {
			in_entry = l == "[Desktop Entry]";
			seen_entry = seen_entry || in_entry;
			continue;
		}
		if (!in_entry)
			continue;
		size_t eq = l.find('=');
		if (eq == std::string_view::npos)
			continue;
		std::string_view key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
		if (key == "Name")
			e.name = value;
		else if (key == "Exec")
			e.exec = strip_field_codes(value);
		else if (key == "Hidden" && is_true(value))
			e.enabled = false;
		else if (key == "X-GNOME-Autostart-enabled" && trim(value) == "false")
			e.enabled = false;
		else if (key == "OnlyShowIn")
			only = split_list(value);
		else if (key == "NotShowIn")
			not_in = split_list(value);
	}
	if (!seen_entry || e.exec.empty())
		return false;
	e.program = startup_name("", e.exec);
	auto here = [&desktops](const std::vector<std::string> &list) {
		for (const std::string &d : list)
			if (std::find(desktops.begin(), desktops.end(), d) != desktops.end())
				return true;
		return false;
	};
	e.for_here = (only.empty() || here(only)) && !here(not_in);
	return true;
}

std::string autostart_set_enabled(std::string_view text, bool on) {
	std::vector<std::string> lines = split_lines(text);
	std::vector<std::string> out;
	bool in_entry = false, set = false;
	size_t header = std::string::npos;
	for (const std::string &raw : lines) {
		std::string_view l = trim(raw);
		if (!l.empty() && l[0] == '[') {
			if (in_entry && !on && !set) {
				out.push_back("Hidden=true");
				set = true;
			}
			in_entry = l == "[Desktop Entry]";
			if (in_entry)
				header = out.size();
			out.push_back(raw);
			continue;
		}
		if (in_entry) {
			size_t eq = l.find('=');
			std::string_view key = eq == std::string_view::npos ? l : trim(l.substr(0, eq));
			if (key == "Hidden") {
				if (!on && !set) {
					out.push_back("Hidden=true");
					set = true;
				}
				continue;  /* on: dropped */
			}
			if (key == "X-GNOME-Autostart-enabled" && on)
				continue;
		}
		out.push_back(raw);
	}
	if (!on && !set) {
		if (header == std::string::npos) {
			out.push_back("[Desktop Entry]");
		}
		/* At the end of the group (it was the last one). */
		out.push_back("Hidden=true");
	}
	std::string s;
	for (size_t i = 0; i < out.size(); ++i)
		s += out[i] + "\n";
	return s;
}

}  // namespace wb
