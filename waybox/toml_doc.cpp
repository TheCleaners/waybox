#include "waybox/toml_doc.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wb::toml {

namespace {

using Type = Value::Type;
using Entry = Document::Entry;
using Section = Document::Section;

bool bare_char(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '_' || c == '-';
}

void append_utf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

bool path_prefix(const Path &prefix, const Path &path) {
	if (prefix.size() > path.size())
		return false;
	for (size_t i = 0; i < prefix.size(); ++i)
		if (prefix[i] != path[i])
			return false;
	return true;
}

/* Reads the text, recording where each key, value and table is. */
struct Reader {
	std::string_view t;
	size_t p = 0;
	std::string error;
	int depth = 0;
	std::vector<Entry> entries;
	std::vector<Section> sections;
	/* [[name]]: how many so far, by name. */
	std::vector<std::pair<Path, size_t>> array_counts;

	bool fail(const std::string &what) {
		if (error.empty()) {
			int line = 1, col = 1;
			for (size_t i = 0; i < p && i < t.size(); ++i) {
				if (t[i] == '\n') {
					++line;
					col = 1;
				} else {
					++col;
				}
			}
			error = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " +
					what;
		}
		return false;
	}

	void spaces() {
		while (p < t.size() && (t[p] == ' ' || t[p] == '\t'))
			++p;
	}

	void comment() {
		if (p < t.size() && t[p] == '#')
			while (p < t.size() && t[p] != '\n')
				++p;
	}

	/* Spaces, comments and line breaks (inside arrays). */
	void blank() {
		for (;;) {
			spaces();
			comment();
			if (p < t.size() && (t[p] == '\n' || t[p] == '\r'))
				++p;
			else
				return;
		}
	}

	/* The rest of a line: only spaces and a comment, then its end. */
	bool line_end() {
		spaces();
		comment();
		if (p < t.size() && t[p] == '\r')
			++p;
		if (p < t.size() && t[p] != '\n')
			return fail("the line goes on after the value (a new line, or a '#' comment)");
		if (p < t.size())
			++p;
		return true;
	}

	bool basic_string(std::string &out, bool multi) {
		p += multi ? 3 : 1;
		if (multi && p < t.size() && t[p] == '\n')
			++p;
		else if (multi && p + 1 < t.size() && t[p] == '\r' && t[p + 1] == '\n')
			p += 2;
		for (;;) {
			if (p >= t.size())
				return fail("a string that doesn't end");
			char c = t[p];
			if (multi && t.compare(p, 3, "\"\"\"") == 0) {
				p += 3;
				/* Up to two more quotes belong to the string. */
				for (int i = 0; i < 2 && p < t.size() && t[p] == '"'; ++i, ++p)
					out += '"';
				return true;
			}
			if (!multi && c == '"') {
				++p;
				return true;
			}
			if (!multi && c == '\n')
				return fail("a line break inside a string");
			++p;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p >= t.size())
				return fail("a string that doesn't end");
			char e = t[p++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'e': out += '\x1b'; break;
			case 'u':
			case 'U': {
				int n = e == 'u' ? 4 : 8;
				if (p + static_cast<size_t>(n) > t.size())
					return fail("a bad \\u escape");
				uint32_t cp = 0;
				for (int i = 0; i < n; ++i) {
					char h = t[p++];
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
					else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
					else return fail("a bad \\u escape");
				}
				if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
					return fail("a \\u escape that isn't a character");
				append_utf8(out, cp);
				break;
			}
			default:
				if (multi && (e == ' ' || e == '\t' || e == '\n' || e == '\r')) {
					/* A line-ending backslash: the break and the indentation go. */
					--p;
					while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' ||
							t[p] == '\r'))
						++p;
					break;
				}
				return fail(std::string("an unknown escape \\") + e);
			}
		}
	}

	bool literal_string(std::string &out, bool multi) {
		p += multi ? 3 : 1;
		if (multi && p < t.size() && t[p] == '\n')
			++p;
		const char *close = multi ? "'''" : "'";
		size_t e = t.find(close, p);
		if (e == std::string_view::npos)
			return fail("a string that doesn't end");
		if (!multi && t.substr(p, e - p).find('\n') != std::string_view::npos)
			return fail("a line break inside a string");
		size_t end = e + (multi ? 3 : 1);
		if (multi)
			while (end < t.size() && t[end] == '\'' && end - e < 5)
				++end;
		out = std::string(t.substr(p, end - (multi ? 3 : 1) - p));
		p = end;
		return true;
	}

	bool key_part(std::string &out) {
		spaces();
		if (p < t.size() && t[p] == '"')
			return t.compare(p, 3, "\"\"\"") != 0 ? basic_string(out, false)
					: fail("a key can't be a multi-line string");
		if (p < t.size() && t[p] == '\'')
			return literal_string(out, false);
		size_t s = p;
		while (p < t.size() && bare_char(t[p]))
			++p;
		if (p == s)
			return fail("a key expected");
		out = std::string(t.substr(s, p - s));
		return true;
	}

	/* a.b."c d" */
	bool key(Path &out) {
		for (;;) {
			std::string part;
			if (!key_part(part))
				return false;
			out.emplace_back(part);
			spaces();
			if (p < t.size() && t[p] == '.') {
				++p;
				continue;
			}
			return true;
		}
	}

	bool number_or_date(Value &v, bool &is_float) {
		size_t s = p;
		while (p < t.size() && (bare_char(t[p]) || t[p] == '+' || t[p] == '.' || t[p] == ':'))
			++p;
		std::string raw(t.substr(s, p - s));
		/* A date and a time with a space between. */
		if (raw.size() == 10 && raw[4] == '-' && p + 1 < t.size() && t[p] == ' ' &&
				t[p + 1] >= '0' && t[p + 1] <= '9') {
			++p;
			while (p < t.size() && (bare_char(t[p]) || t[p] == '+' || t[p] == '.' ||
					t[p] == ':'))
				++p;
			raw = std::string(t.substr(s, p - s));
		}
		if (raw.empty())
			return fail("a value expected");
		std::string digits;
		for (char c : raw)
			if (c != '_')
				digits += c;
		bool date = raw.size() >= 5 && ((raw[4] == '-' && raw[0] >= '0' && raw[0] <= '9') ||
				raw[2] == ':');
		if (date) {
			v.type = Type::String;
			v.string = raw;
			return true;
		}
		std::string body = digits[0] == '+' || digits[0] == '-' ? digits.substr(1) : digits;
		if (body == "inf" || body == "nan") {
			v.type = Type::Number;
			v.number = body == "inf" ? HUGE_VAL : NAN;
			if (digits[0] == '-')
				v.number = -v.number;
			is_float = true;
			return true;
		}
		errno = 0;
		char *end = nullptr;
		if (body.rfind("0x", 0) == 0 || body.rfind("0o", 0) == 0 || body.rfind("0b", 0) == 0) {
			int base = body[1] == 'x' ? 16 : body[1] == 'o' ? 8 : 2;
			long long i = std::strtoll(body.c_str() + 2, &end, base);
			if (errno || !end || *end || body.size() == 2)
				return fail("a number that isn't one: " + raw);
			v.type = Type::Integer;
			v.integer = i;
			return true;
		}
		if (digits.find_first_of(".eE") == std::string::npos) {
			long long i = std::strtoll(digits.c_str(), &end, 10);
			if (errno || !end || *end)
				return fail("a number that isn't one: " + raw);
			v.type = Type::Integer;
			v.integer = i;
			return true;
		}
		double d = std::strtod(digits.c_str(), &end);
		if (!end || *end)
			return fail("a number that isn't one: " + raw);
		v.type = Type::Number;
		v.number = d;
		is_float = true;
		return true;
	}

	/* Inline table members are recorded (to set and remove in place) under
	 * `base`; `section` is the table they're in. */
	bool value(Value &v, bool &is_float, const Path &base, size_t section) {
		if (p >= t.size())
			return fail("a value expected");
		if (++depth > 100)
			return fail("nested too deeply");
		char c = t[p];
		bool good = true;
		if (t.compare(p, 3, "\"\"\"") == 0) {
			v.type = Type::String;
			good = basic_string(v.string, true);
		} else if (c == '"') {
			v.type = Type::String;
			good = basic_string(v.string, false);
		} else if (t.compare(p, 3, "'''") == 0) {
			v.type = Type::String;
			good = literal_string(v.string, true);
		} else if (c == '\'') {
			v.type = Type::String;
			good = literal_string(v.string, false);
		} else if (t.compare(p, 4, "true") == 0 && (p + 4 >= t.size() || !bare_char(t[p + 4]))) {
			v.type = Type::Bool;
			v.boolean = true;
			p += 4;
		} else if (t.compare(p, 5, "false") == 0 && (p + 5 >= t.size() || !bare_char(t[p + 5]))) {
			v.type = Type::Bool;
			p += 5;
		} else if (c == '[') {
			v.type = Type::Array;
			++p;
			for (;;) {
				blank();
				if (p < t.size() && t[p] == ']') {
					++p;
					break;
				}
				Value item;
				bool f = false;
				if (!value(item, f, {}, section))
					return false;
				v.items.push_back(std::move(item));
				blank();
				if (p < t.size() && t[p] == ',') {
					++p;
				} else if (p < t.size() && t[p] == ']') {
					++p;
					break;
				} else {
					return fail("',' or ']' expected");
				}
			}
		} else if (c == '{') {
			v.type = Type::Object;
			++p;
			bool record = !base.empty();
			for (;;) {
				blank();
				if (p < t.size() && t[p] == '}') {
					++p;
					break;
				}
				Entry e;
				e.key_start = p;
				e.line_start = p;
				e.inline_member = true;
				e.section = section;
				Path k;
				if (!key(k))
					return false;
				spaces();
				if (p >= t.size() || t[p] != '=')
					return fail("'=' expected after the key");
				++p;
				spaces();
				e.value_start = p;
				Value mv;
				bool f = false;
				Path full = base;
				full.insert(full.end(), k.begin(), k.end());
				if (!value(mv, f, record ? full : Path{}, section))
					return false;
				e.value_end = p;
				e.line_end = p;
				e.type = mv.type;
				e.is_float = f;
				e.is_inline_table = mv.type == Type::Object && t[e.value_start] == '{';
				e.path = full;
				if (record)
					entries.push_back(e);
				/* a.b = 1 inside: nested objects. */
				Value *at = &v;
				for (size_t i = 0; i + 1 < k.size(); ++i) {
					const std::string &name = std::get<std::string>(k[i]);
					Value *next = nullptr;
					for (auto &m : at->members)
						if (m.first == name)
							next = &m.second;
					if (!next) {
						Value o;
						o.type = Type::Object;
						at->members.emplace_back(name, std::move(o));
						next = &at->members.back().second;
					}
					at = next;
				}
				at->members.emplace_back(std::get<std::string>(k.back()), std::move(mv));
				blank();
				if (p < t.size() && t[p] == ',') {
					++p;
				} else if (p < t.size() && t[p] == '}') {
					++p;
					break;
				} else {
					return fail("',' or '}' expected");
				}
			}
		} else {
			good = number_or_date(v, is_float);
		}
		--depth;
		return good;
	}

	bool document() {
		sections.push_back({});
		for (;;) {
			blank();
			if (p >= t.size())
				return true;
			if (t[p] == '[') {
				Section s;
				s.header_start = p;
				s.array = t.compare(p, 2, "[[") == 0;
				p += s.array ? 2 : 1;
				Path k;
				if (!key(k))
					return false;
				spaces();
				if (t.compare(p, s.array ? 2 : 1, s.array ? "]]" : "]") != 0)
					return fail(s.array ? "']]' expected" : "']' expected");
				p += s.array ? 2 : 1;
				if (s.array) {
					size_t n = 0;
					bool found = false;
					for (auto &[name, count] : array_counts)
						if (name == k) {
							n = count++;
							found = true;
						}
					if (!found)
						array_counts.emplace_back(k, 1);
					/* Inside an earlier array's element: its latest. */
					k.push_back(n);
				}
				/* [a.b] under [[a]]: a's latest element. */
				Path resolved;
				for (size_t i = 0; i < k.size(); ++i) {
					resolved.push_back(k[i]);
					if (i + 1 < k.size() && std::holds_alternative<std::string>(k[i + 1]))
						for (auto &[name, count] : array_counts)
							if (name == resolved) {
								resolved.push_back(count - 1);
								break;
							}
				}
				s.path = resolved;
				if (!line_end())
					return false;
				s.header_end = p;
				sections.push_back(std::move(s));
				continue;
			}
			Entry e;
			e.line_start = p;
			while (e.line_start > 0 && (t[e.line_start - 1] == ' ' || t[e.line_start - 1] == '\t'))
				--e.line_start;
			e.key_start = p;
			e.section = sections.size() - 1;
			Path k;
			if (!key(k))
				return false;
			spaces();
			if (p >= t.size() || t[p] != '=')
				return fail("'=' expected after the key");
			++p;
			spaces();
			e.path = sections.back().path;
			e.path.insert(e.path.end(), k.begin(), k.end());
			e.value_start = p;
			Value v;
			bool f = false;
			size_t at_entry = entries.size();
			entries.push_back(e);
			if (!value(v, f, e.path, e.section))
				return false;
			Entry &me = entries[at_entry];
			me.value_end = p;
			me.type = v.type;
			me.is_float = f;
			me.is_inline_table = v.type == Type::Object;
			if (!line_end())
				return false;
			me.line_end = p;
		}
	}
};

Value *walk(Value &root, const Path &path, bool make) {
	Value *at = &root;
	for (const auto &part : path) {
		if (const auto *k = std::get_if<std::string>(&part)) {
			if (at->type != Type::Object) {
				if (!make || at->type != Type::Null)
					return nullptr;
				at->type = Type::Object;
			}
			Value *next = nullptr;
			for (auto &m : at->members)
				if (m.first == *k)
					next = &m.second;
			if (!next) {
				if (!make)
					return nullptr;
				at->members.emplace_back(*k, Value{});
				next = &at->members.back().second;
			}
			at = next;
		} else {
			size_t i = std::get<size_t>(part);
			if (at->type != Type::Array) {
				if (!make || at->type != Type::Null)
					return nullptr;
				at->type = Type::Array;
			}
			while (make && at->items.size() <= i) {
				Value o;
				o.type = Type::Object;
				at->items.push_back(o);
			}
			if (i >= at->items.size())
				return nullptr;
			at = &at->items[i];
		}
	}
	return at;
}

std::string number_text(double d) {
	if (std::isnan(d))
		return "nan";
	if (std::isinf(d))
		return d < 0 ? "-inf" : "inf";
	char buf[40];
	for (int prec = 15; prec <= 17; ++prec) {
		std::snprintf(buf, sizeof buf, "%.*g", prec, d);
		if (std::strtod(buf, nullptr) == d)
			break;
	}
	std::string s = buf;
	if (s.find_first_of(".eE") == std::string::npos)
		s += ".0";
	return s;
}

std::string quote(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\t': out += "\\t"; break;
		case '\r': out += "\\r"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

bool holds_null(const Value &v) {
	if (v.type == Type::Null)
		return true;
	for (const Value &i : v.items)
		if (holds_null(i))
			return true;
	for (const auto &m : v.members)
		if (holds_null(m.second))
			return true;
	return false;
}

std::string dotted(const Path &path, size_t from) {
	std::string out;
	for (size_t i = from; i < path.size(); ++i)
		out += (i > from ? "." : "") + key_text(std::get<std::string>(path[i]));
	return out;
}

}  // namespace

std::string key_text(const std::string &key) {
	bool bare = !key.empty();
	for (char c : key)
		bare = bare && bare_char(c);
	return bare ? key : quote(key);
}

std::string serialize(const Value &v) {
	switch (v.type) {
	case Type::Null: return "";
	case Type::Bool: return v.boolean ? "true" : "false";
	case Type::Integer: return std::to_string(v.integer);
	case Type::Number: return number_text(v.number);
	case Type::String: return quote(v.string);
	case Type::Array: {
		std::string out = "[";
		for (size_t i = 0; i < v.items.size(); ++i)
			out += (i ? ", " : "") + toml::serialize(v.items[i]);
		return out + "]";
	}
	case Type::Object: {
		if (v.members.empty())
			return "{}";
		std::string out = "{ ";
		for (size_t i = 0; i < v.members.size(); ++i)
			out += (i ? ", " : "") + key_text(v.members[i].first) + " = " +
					toml::serialize(v.members[i].second);
		return out + " }";
	}
	}
	return "";
}

Document Document::parse(std::string_view text) {
	Document d;
	d.text_ = std::string(text);
	d.reparse();
	return d;
}

void Document::reparse() {
	entries_.clear();
	sections_.clear();
	tree_ = Value{};
	tree_.type = Type::Object;
	error_.clear();
	Reader r;
	r.t = text_;
	if (!r.document()) {
		error_ = r.error;
		return;
	}
	entries_ = std::move(r.entries);
	sections_ = std::move(r.sections);
	for (const Section &s : sections_)
		walk(tree_, s.path, true);
	/* Each key's value, from the text (an inline table's members are in
	 * its own value). */
	for (const Entry &e : entries_) {
		if (e.inline_member)
			continue;
		Reader vr;
		vr.t = text_;
		vr.p = e.value_start;
		Value v;
		bool f = false;
		vr.value(v, f, {}, 0);
		if (Value *at = walk(tree_, e.path, true))
			*at = std::move(v);
	}
}

std::optional<Value> Document::get(const Path &path) const {
	if (!ok())
		return std::nullopt;
	Value copy = tree_;
	Value *at = walk(copy, path, false);
	if (!at)
		return std::nullopt;
	return *at;
}

bool Document::set(const Path &path, const Value &value) {
	if (!ok() || path.empty() || holds_null(value) ||
			!std::holds_alternative<std::string>(path.back()))
		return false;
	/* 1. A key that's there: its value, in place. */
	for (const Entry &e : entries_)
		if (e.path == path) {
			Value v = value;
			if (e.is_float && v.type == Type::Integer) {
				v.type = Type::Number;  /* 12 into size = 11.0: 12.0 */
				v.number = static_cast<double>(v.integer);
			}
			text_.replace(e.value_start, e.value_end - e.value_start, toml::serialize(v));
			reparse();
			return ok();
		}
	/* Through a value that isn't a table: no. */
	for (const Entry &e : entries_)
		if (path_prefix(e.path, path) && e.path.size() < path.size() && !e.is_inline_table)
			return false;
	/* 2. Inside an inline table: a member added. */
	for (const Entry &e : entries_) {
		if (!e.is_inline_table || !path_prefix(e.path, path) || e.path.size() >= path.size())
			continue;
		bool deeper = false;  /* a nested inline table nearer: that one */
		for (const Entry &o : entries_)
			if (o.is_inline_table && o.path.size() > e.path.size() &&
					path_prefix(o.path, path) && o.path.size() < path.size())
				deeper = true;
		if (deeper)
			continue;
		std::string member = dotted(path, e.path.size()) + " = " + toml::serialize(value);
		size_t close = e.value_end - 1;  /* '}' */
		size_t content = close;
		while (content > e.value_start + 1 && (text_[content - 1] == ' ' ||
				text_[content - 1] == '\t' || text_[content - 1] == '\n'))
			--content;
		if (content == e.value_start + 1)
			text_.replace(e.value_start, e.value_end - e.value_start, "{ " + member + " }");
		else if (text_[content - 1] == ',')
			text_.insert(content, " " + member);
		else
			text_.insert(content, ", " + member);
		reparse();
		return ok();
	}
	/* 3. In the nearest table with a [header] (or the top of the file). */
	size_t best = 0;
	for (size_t i = 0; i < sections_.size(); ++i)
		if (path_prefix(sections_[i].path, path) && sections_[i].path.size() < path.size() &&
				sections_[i].path.size() >= sections_[best].path.size())
			best = i;
	const Section &s = sections_[best];
	for (size_t i = s.path.size(); i + 1 < path.size(); ++i)
		if (!std::holds_alternative<std::string>(path[i]))
			return false;  /* a new element of an array of tables */
	size_t rest = path.size() - s.path.size();
	/* Deeper than one key: its own [header] at the end, unless the table
	 * in between is already made of dotted keys here (a header for it
	 * then isn't allowed). */
	bool dotted_here = false;
	if (rest > 1) {
		Path first(path.begin(), path.begin() + static_cast<long>(s.path.size() + 1));
		for (const Entry &e : entries_)
			if (!e.inline_member && e.section == best && path_prefix(first, e.path))
				dotted_here = true;
	}
	/* Inside an element of an array of tables ([[bind]] 0): a [header]
	 * can't name it (one at the end would be the last element's), so a
	 * dotted key in the element itself. */
	bool through_array = false;
	for (size_t i = 0; i < s.path.size(); ++i)
		if (!std::holds_alternative<std::string>(path[i]))
			through_array = true;
	if (rest > 1 && !dotted_here && !through_array) {
		std::string header = "[" + dotted(Path(path.begin(), path.end() - 1), 0) + "]";
		std::string add;
		if (!text_.empty() && text_.back() != '\n')
			add += "\n";
		if (!text_.empty())
			add += "\n";
		add += header + "\n" + key_text(std::get<std::string>(path.back())) + " = " +
				toml::serialize(value) + "\n";
		text_ += add;
		reparse();
		return ok();
	}
	std::string line = dotted(path, s.path.size()) + " = " + toml::serialize(value);
	/* After its table's last key, in its indentation. */
	const Entry *last = nullptr;
	for (const Entry &e : entries_)
		if (!e.inline_member && e.section == best)
			last = &e;
	size_t at;
	std::string indent;
	if (last) {
		at = last->line_end;
		indent = text_.substr(last->line_start, last->key_start - last->line_start);
	} else if (best > 0) {
		at = s.header_end;
	} else {
		/* The top, no keys yet: before the first [header] and the comments
		 * and blank lines just above it (they're about the table); with no
		 * [header], at the end. */
		at = text_.size();
		if (sections_.size() > 1) {
			at = sections_[1].header_start;
			while (at > 0) {
				size_t ls = at - 1;  /* the previous line's '\n' */
				while (ls > 0 && text_[ls - 1] != '\n')
					--ls;
				std::string_view l(text_.data() + ls, at - 1 - ls);
				size_t nb = l.find_first_not_of(" \t\r");
				if (nb != std::string_view::npos && l[nb] != '#')
					break;
				at = ls;
			}
		}
	}
	std::string add = indent + line + "\n";
	if (at > 0 && text_[at - 1] != '\n')
		add = "\n" + add;
	text_.insert(at, add);
	reparse();
	return ok();
}

bool Document::remove(const Path &path) {
	if (!ok())
		return false;
	for (const Entry &e : entries_) {
		if (e.path != path)
			continue;
		if (!e.inline_member) {
			text_.erase(e.line_start, e.line_end - e.line_start);
		} else {
			/* "k = v, " or ", k = v"; the only one: "{}" */
			size_t s = e.key_start, end = e.value_end;
			size_t open = s, close = end;
			while (open > 0 && (text_[open - 1] == ' ' || text_[open - 1] == '\t'))
				--open;
			while (close < text_.size() && (text_[close] == ' ' || text_[close] == '\t'))
				++close;
			if (open > 0 && text_[open - 1] == '{' && close < text_.size() &&
					text_[close] == '}') {
				text_.erase(open, close - open);
				reparse();
				return ok();
			}
			size_t q = end;
			while (q < text_.size() && (text_[q] == ' ' || text_[q] == '\t'))
				++q;
			if (q < text_.size() && text_[q] == ',') {
				end = q + 1;
				while (end < text_.size() && (text_[end] == ' ' || text_[end] == '\t'))
					++end;
			} else {
				size_t b = s;
				while (b > 0 && (text_[b - 1] == ' ' || text_[b - 1] == '\t'))
					--b;
				if (b > 0 && text_[b - 1] == ',')
					s = b - 1;
			}
			text_.erase(s, end - s);
		}
		reparse();
		return ok();
	}
	return false;
}

}  // namespace wb::toml
