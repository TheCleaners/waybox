#include "waybox/jsonc_doc.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wb::jsonc {

namespace {

using Node = Document::Node;
using Type = Value::Type;

/* A recursive-descent reader that records where everything is. */
struct Reader {
	std::string_view t;
	size_t p = 0;
	std::string error;
	int depth = 0;

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

	/* Whitespace and comments. */
	bool skip() {
		while (p < t.size()) {
			char c = t[p];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				++p;
			} else if (c == '/' && p + 1 < t.size() && t[p + 1] == '/') {
				while (p < t.size() && t[p] != '\n')
					++p;
			} else if (c == '/' && p + 1 < t.size() && t[p + 1] == '*') {
				size_t e = t.find("*/", p + 2);
				if (e == std::string_view::npos)
					return fail("a comment that doesn't end");
				p = e + 2;
			} else {
				break;
			}
		}
		return true;
	}

	bool string(std::string &out) {
		++p;  /* the quote */
		while (p < t.size() && t[p] != '"') {
			char c = t[p++];
			if (static_cast<unsigned char>(c) < 0x20)
				return fail("a line break inside a string");
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p >= t.size())
				break;
			char e = t[p++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				auto hex4 = [this](uint32_t &v) {
					if (p + 4 > t.size())
						return false;
					v = 0;
					for (int i = 0; i < 4; ++i) {
						char h = t[p++];
						v <<= 4;
						if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
						else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
						else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
						else return false;
					}
					return true;
				};
				uint32_t cp;
				if (!hex4(cp))
					return fail("a bad \\u escape");
				if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < t.size() && t[p] == '\\' &&
						t[p + 1] == 'u') {
					p += 2;
					uint32_t lo;
					if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
						return fail("a bad \\u escape");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
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
				break;
			}
			default:
				return fail(std::string("an unknown escape \\") + e);
			}
		}
		if (p >= t.size())
			return fail("a string that doesn't end");
		++p;
		return true;
	}

	bool value(Node &n) {
		if (!skip())
			return false;
		if (p >= t.size())
			return fail("a value missing");
		if (++depth > 200)
			return fail("nested too deeply");
		n.start = p;
		char c = t[p];
		bool good = true;
		if (c == '{') {
			n.type = Type::Object;
			++p;
			for (;;) {
				if (!skip())
					return false;
				if (p < t.size() && t[p] == '}') {
					++p;
					break;
				}
				if (p >= t.size() || t[p] != '"')
					return fail("a key (in double quotes) or '}' expected");
				Node::Member m;
				m.key_start = p;
				if (!string(m.key) || !skip())
					return false;
				if (p >= t.size() || t[p] != ':')
					return fail("':' expected after the key");
				++p;
				if (!value(m.node))
					return false;
				n.members.push_back(std::move(m));
				if (!skip())
					return false;
				if (p < t.size() && t[p] == ',') {
					++p;
				} else if (p < t.size() && t[p] == '}') {
					++p;
					break;
				} else {
					return fail("',' or '}' expected");
				}
			}
		} else if (c == '[') {
			n.type = Type::Array;
			++p;
			for (;;) {
				if (!skip())
					return false;
				if (p < t.size() && t[p] == ']') {
					++p;
					break;
				}
				Node item;
				if (!value(item))
					return false;
				n.items.push_back(std::move(item));
				if (!skip())
					return false;
				if (p < t.size() && t[p] == ',') {
					++p;
				} else if (p < t.size() && t[p] == ']') {
					++p;
					break;
				} else {
					return fail("',' or ']' expected");
				}
			}
		} else if (c == '"') {
			n.type = Type::String;
			good = string(n.scalar);
		} else if (t.compare(p, 4, "true") == 0 || t.compare(p, 5, "false") == 0) {
			n.type = Type::Bool;
			n.scalar = t[p] == 't' ? "true" : "false";
			p += n.scalar.size();
		} else if (t.compare(p, 4, "null") == 0) {
			n.type = Type::Null;
			p += 4;
		} else if (c == '-' || (c >= '0' && c <= '9')) {
			size_t s = p;
			if (t[p] == '-')
				++p;
			bool integer = true;
			while (p < t.size() && ((t[p] >= '0' && t[p] <= '9') || t[p] == '.' ||
					t[p] == 'e' || t[p] == 'E' || t[p] == '+' || t[p] == '-')) {
				if (t[p] == '.' || t[p] == 'e' || t[p] == 'E')
					integer = false;
				++p;
			}
			n.scalar = std::string(t.substr(s, p - s));
			n.type = integer ? Type::Integer : Type::Number;
		} else {
			return fail(std::string("a value expected, not '") + c + "'");
		}
		--depth;
		n.end = p;
		return good;
	}
};

Value to_value(const Node &n) {
	Value v;
	v.type = n.type;
	switch (n.type) {
	case Type::Null: break;
	case Type::Bool: v.boolean = n.scalar == "true"; break;
	case Type::Integer: {
		errno = 0;
		char *end = nullptr;
		long long i = std::strtoll(n.scalar.c_str(), &end, 10);
		if (errno == 0 && end && *end == '\0') {
			v.integer = i;
		} else {
			v.type = Type::Number;
			v.number = std::strtod(n.scalar.c_str(), nullptr);
		}
		break;
	}
	case Type::Number: v.number = std::strtod(n.scalar.c_str(), nullptr); break;
	case Type::String: v.string = n.scalar; break;
	case Type::Array:
		for (const Node &i : n.items)
			v.items.push_back(to_value(i));
		break;
	case Type::Object:
		for (const Node::Member &m : n.members) {
			Value mv = to_value(m.node);
			bool replaced = false;
			for (auto &[k, old] : v.members)
				if (k == m.key) {
					old = std::move(mv);  /* a repeated key: the last counts */
					replaced = true;
					break;
				}
			if (!replaced)
				v.members.emplace_back(m.key, std::move(mv));
		}
		break;
	}
	return v;
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
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", c);
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

bool scalar(const Value &v) {
	return v.type != Type::Array && v.type != Type::Object;
}

/* The whitespace a line starts with. */
std::string line_indent(std::string_view t, size_t at) {
	size_t ls = t.rfind('\n', at == 0 ? 0 : at - 1);
	ls = ls == std::string_view::npos || at == 0 ? 0 : ls + 1;
	size_t e = ls;
	while (e < t.size() && (t[e] == ' ' || t[e] == '\t'))
		++e;
	return std::string(t.substr(ls, e - ls));
}

/* Only spaces and tabs between the line's start and `at`. */
bool starts_line(std::string_view t, size_t at) {
	for (size_t i = at; i > 0; --i) {
		char c = t[i - 1];
		if (c == '\n')
			return true;
		if (c != ' ' && c != '\t')
			return false;
	}
	return true;
}

/* After `at`: spaces, then a comma? Its position, or npos. */
size_t comma_after(std::string_view t, size_t at) {
	while (at < t.size() && (t[at] == ' ' || t[at] == '\t' || t[at] == '\n' || t[at] == '\r'))
		++at;
	return at < t.size() && t[at] == ',' ? at : std::string_view::npos;
}

/* The end of the line `at` is on, if the rest of it is only spaces and a
 * // comment (where a new line can go); else npos. */
size_t quiet_line_end(std::string_view t, size_t at) {
	size_t i = at;
	while (i < t.size() && (t[i] == ' ' || t[i] == '\t'))
		++i;
	if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/')
		i = t.find('\n', i);
	else if (i < t.size() && t[i] != '\n' && t[i] != '\r')
		return std::string_view::npos;
	return i == std::string_view::npos ? t.size() : i;
}

bool same_scalar(const Value &a, const Value &b) {
	if (a.type != b.type)
		return false;
	switch (a.type) {
	case Type::Null: return true;
	case Type::Bool: return a.boolean == b.boolean;
	case Type::Integer: return a.integer == b.integer;
	case Type::Number: return a.number == b.number;
	case Type::String: return a.string == b.string;
	default: return false;
	}
}

/* An array one item a line (or with comments in it), replaced by another
 * of scalars: line by line, so what's in it stays. An item kept keeps its
 * line (its comment after it too); comment lines go with the item below
 * them, or stay where they were when it's gone; a new item gets the
 * items' indentation. False (replace it whole): not laid out so. */
bool rewrite_lines(const std::string &t, const Node &old, const Value &value,
		std::string *out) {
	if (old.type != Type::Array || value.type != Type::Array || old.items.empty())
		return false;
	for (const Value &v : value.items)
		if (!scalar(v))
			return false;
	std::string_view span(t.data() + old.start, old.end - old.start);
	if (span.find('\n') == std::string_view::npos)
		return false;
	for (const Node &it : old.items)
		if (it.type == Type::Array || it.type == Type::Object || !starts_line(t, it.start))
			return false;
	size_t close = old.end - 1;  /* ']' */
	if (!starts_line(t, close))
		return false;
	auto line_start = [&t](size_t at) {
		while (at > 0 && t[at - 1] != '\n')
			--at;
		return at;
	};
	auto after_line = [&t](size_t at) {  /* past the line's '\n' */
		size_t nl = t.find('\n', at);
		return nl == std::string::npos ? t.size() : nl + 1;
	};
	/* '[' and the rest of its line. */
	size_t first_line = line_start(old.items[0].start);
	size_t head_end = after_line(old.start);
	if (head_end > first_line)
		return false;  /* an item on the '[' line */
	struct Item {
		std::string lead;    /* comment and blank lines above it */
		std::string indent, text, after;  /* its line: the value, then a comment */
		Value value;
		bool used = false;
	};
	std::vector<Item> items;
	size_t prev = head_end;
	bool trailing_comma = false;
	for (size_t k = 0; k < old.items.size(); ++k) {
		const Node &it = old.items[k];
		Item item;
		size_t ls = line_start(it.start);
		item.lead = t.substr(prev, ls - prev);
		item.indent = t.substr(ls, it.start - ls);
		item.text = t.substr(it.start, it.end - it.start);
		size_t rest = it.end;
		size_t comma = comma_after(t, it.end);
		size_t eol = t.find('\n', it.end);
		if (eol == std::string::npos)
			eol = t.size();
		if (comma != std::string::npos && comma < eol) {
			rest = comma + 1;
			if (k + 1 == old.items.size())
				trailing_comma = true;
		}
		item.after = t.substr(rest, eol - rest);
		/* Another item, or the ']', on its line: not one a line. */
		if (item.after.find_first_not_of(" \t") != std::string::npos &&
				item.after.find("//") == std::string::npos &&
				item.after.find("/*") == std::string::npos)
			return false;
		item.value = to_value(it);
		prev = after_line(it.end);
		items.push_back(std::move(item));
	}
	std::string tail = t.substr(prev, line_start(close) - prev);
	std::string closer = t.substr(line_start(close), old.end - line_start(close));
	std::string indent = items[0].indent;
	/* Each new item: an old one with its value (its line), or a new line. */
	std::vector<int> from(value.items.size(), -1);
	for (size_t j = 0; j < value.items.size(); ++j)
		for (size_t k = 0; k < items.size(); ++k)
			if (!items[k].used && same_scalar(items[k].value, value.items[j])) {
				items[k].used = true;
				from[j] = static_cast<int>(k);
				break;
			}
	/* A gone item's comment lines: with the next one kept, else at the end. */
	std::string pending;
	for (Item &item : items) {
		if (item.used) {
			item.lead = pending + item.lead;
			pending.clear();
		} else {
			pending += item.lead;
		}
	}
	std::string s = t.substr(old.start, head_end - old.start);
	for (size_t j = 0; j < value.items.size(); ++j) {
		bool last = j + 1 == value.items.size();
		std::string comma = !last || trailing_comma ? "," : "";
		if (from[j] >= 0) {
			const Item &item = items[static_cast<size_t>(from[j])];
			s += item.lead + item.indent + item.text + comma + item.after + "\n";
		} else {
			s += indent + serialize(value.items[j]) + comma + "\n";
		}
	}
	s += pending + tail + closer;
	*out = s;
	return true;
}

}  // namespace

std::string serialize(const Value &v, const std::string &indent, const std::string &unit) {
	switch (v.type) {
	case Type::Null: return "null";
	case Type::Bool: return v.boolean ? "true" : "false";
	case Type::Integer: return std::to_string(v.integer);
	case Type::Number: {
		if (!std::isfinite(v.number))
			return "null";
		char buf[40];
		for (int prec = 15; prec <= 17; ++prec) {
			std::snprintf(buf, sizeof buf, "%.*g", prec, v.number);
			if (std::strtod(buf, nullptr) == v.number)
				break;
		}
		std::string s = buf;
		if (s.find_first_of(".eEn") == std::string::npos)
			s += ".0";
		return s;
	}
	case Type::String: return quote(v.string);
	case Type::Array: {
		if (v.items.empty())
			return "[]";
		bool flat = true;
		for (const Value &i : v.items)
			flat = flat && scalar(i);
		std::string out = "[";
		for (size_t i = 0; i < v.items.size(); ++i) {
			if (flat)
				out += (i ? ", " : "") + serialize(v.items[i]);
			else
				out += (i ? ",\n" : "\n") + indent + unit +
						serialize(v.items[i], indent + unit, unit);
		}
		return out + (flat ? "]" : "\n" + indent + "]");
	}
	case Type::Object: {
		if (v.members.empty())
			return "{}";
		std::string out = "{";
		for (size_t i = 0; i < v.members.size(); ++i)
			out += (i ? ",\n" : "\n") + indent + unit + quote(v.members[i].first) + ": " +
					serialize(v.members[i].second, indent + unit, unit);
		return out + "\n" + indent + "}";
	}
	}
	return "null";
}

Document Document::parse(std::string_view text) {
	Document d;
	d.text_ = std::string(text);
	d.reparse();
	return d;
}

void Document::reparse() {
	root_.reset();
	error_.clear();
	Reader r;
	r.t = text_;
	if (!r.skip()) {
		error_ = r.error;
		return;
	}
	if (r.p >= text_.size())
		return;  /* nothing yet */
	Node n;
	if (!r.value(n) || !r.skip()) {
		error_ = r.error;
		return;
	}
	if (r.p < text_.size()) {
		r.fail("something after the end");
		error_ = r.error;
		return;
	}
	root_ = std::move(n);
}

const Node *Document::find(const Path &path) const {
	if (!root_)
		return nullptr;
	const Node *n = &*root_;
	for (const PathPart &part : path) {
		if (const auto *key = std::get_if<std::string>(&part)) {
			if (n->type != Type::Object)
				return nullptr;
			const Node *found = nullptr;
			for (const Node::Member &m : n->members)
				if (m.key == *key)
					found = &m.node;  /* the last, if repeated */
			if (!found)
				return nullptr;
			n = found;
		} else {
			size_t i = std::get<size_t>(part);
			if (n->type != Type::Array || i >= n->items.size())
				return nullptr;
			n = &n->items[i];
		}
	}
	return n;
}

std::optional<Value> Document::get(const Path &path) const {
	const Node *n = find(path);
	if (!n)
		return std::nullopt;
	return to_value(*n);
}

void Document::edit(size_t at, size_t len, const std::string &with) {
	text_.replace(at, len, with);
}

std::string Document::indent_unit() const {
	/* The first indented line's indentation. */
	size_t p = 0;
	while ((p = text_.find('\n', p)) != std::string::npos) {
		++p;
		size_t e = p;
		while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t'))
			++e;
		if (e > p && e < text_.size() && text_[e] != '\n' && text_[e] != '\r')
			return text_.substr(p, e - p);
	}
	return "    ";
}

bool Document::set(const Path &path, const Value &value) {
	if (!ok())
		return false;
	std::string unit = indent_unit();
	if (!root_) {
		/* Nothing yet: an object, or the value itself; on a line of its
		 * own (not inside a // comment the file ends with). */
		if (!text_.empty() && text_.back() != '\n')
			text_ += "\n";
		if (path.empty()) {
			text_ += serialize(value, "", unit) + "\n";
			reparse();
			return ok();
		}
		if (!std::holds_alternative<std::string>(path[0]))
			return false;
		text_ += "{}\n";
		reparse();
	}
	/* The deepest part of the path that's there. */
	size_t have = path.size();
	while (have > 0 && !find(Path(path.begin(), path.begin() + static_cast<long>(have))))
		--have;
	if (have == path.size()) {
		const Node *n = find(path);
		std::string kept;
		if (rewrite_lines(text_, *n, value, &kept))
			edit(n->start, n->end - n->start, kept);
		else
			edit(n->start, n->end - n->start,
					serialize(value, line_indent(text_, n->start), unit));
		reparse();
		return ok();
	}
	/* Add path[have] to the container at path[:have], the rest of the path
	 * as objects around the value. */
	const Node *parent = find(Path(path.begin(), path.begin() + static_cast<long>(have)));
	if (!parent)
		return false;
	const PathPart &next = path[have];
	Value add = value;
	for (size_t i = path.size(); i-- > have + 1;) {
		if (!std::holds_alternative<std::string>(path[i]))
			return false;
		Value wrap;
		wrap.type = Type::Object;
		wrap.members.emplace_back(std::get<std::string>(path[i]), std::move(add));
		add = std::move(wrap);
	}
	std::string prefix;
	size_t count;
	size_t open = parent->start;
	if (parent->type == Type::Object && std::holds_alternative<std::string>(next)) {
		prefix = quote(std::get<std::string>(next)) + ": ";
		count = parent->members.size();
	} else if (parent->type == Type::Array && std::holds_alternative<size_t>(next) &&
			std::get<size_t>(next) == parent->items.size()) {
		count = parent->items.size();
	} else {
		return false;
	}
	if (count == 0) {
		/* "{}" or "{\n}": a line of its own, inside. */
		std::string outer = line_indent(text_, open);
		std::string inner = outer + unit;
		bool multiline = text_.substr(open, parent->end - open).find('\n') != std::string::npos;
		std::string line = "\n" + inner + prefix + serialize(add, inner, unit) +
				(multiline ? "" : "\n" + outer);
		edit(open + 1, 0, line);
		reparse();
		return ok();
	}
	size_t last_start = parent->type == Type::Object ? parent->members.back().key_start
			: parent->items.back().start;
	size_t last_end = parent->type == Type::Object ? parent->members.back().node.end
			: parent->items.back().end;
	size_t comma = comma_after(text_, last_end);
	bool own_lines = starts_line(text_, last_start);
	if (own_lines) {
		std::string ind = line_indent(text_, last_start);
		std::string entry = prefix + serialize(add, ind, unit);
		/* After the last one's line (past a // comment on it). */
		size_t after = comma != std::string::npos ? comma + 1 : last_end;
		size_t eol = quiet_line_end(text_, after);
		if (eol == std::string::npos)
			eol = after;
		if (comma != std::string::npos) {
			/* A trailing comma: the file's style, kept. */
			edit(eol, 0, "\n" + ind + entry + ",");
		} else {
			edit(eol, 0, "\n" + ind + entry);
			edit(last_end, 0, ",");
		}
	} else {
		std::string entry = prefix + serialize(add, line_indent(text_, last_start), unit);
		if (comma != std::string::npos)
			edit(comma + 1, 0, " " + entry + ",");
		else
			edit(last_end, 0, ", " + entry);
	}
	reparse();
	return ok();
}

bool Document::remove(const Path &path) {
	if (!ok() || path.empty())
		return false;
	const Node *parent = find(Path(path.begin(), path.end() - 1));
	if (!parent)
		return false;
	const PathPart &last = path.back();
	/* Every member of that key (a repeated key would come back otherwise);
	 * from the end, so earlier positions hold. */
	std::vector<size_t> which;
	size_t count = 0;
	if (parent->type == Type::Object && std::holds_alternative<std::string>(last)) {
		count = parent->members.size();
		for (size_t i = 0; i < count; ++i)
			if (parent->members[i].key == std::get<std::string>(last))
				which.push_back(i);
	} else if (parent->type == Type::Array && std::holds_alternative<size_t>(last) &&
			std::get<size_t>(last) < parent->items.size()) {
		count = parent->items.size();
		which.push_back(std::get<size_t>(last));
	}
	if (which.empty())
		return false;
	Path parent_path(path.begin(), path.end() - 1);
	for (size_t k = which.size(); k-- > 0;) {
		parent = find(parent_path);
		size_t i = which[k];
		count = parent->type == Type::Object ? parent->members.size() : parent->items.size();
		auto start_of = [&](size_t j) {
			return parent->type == Type::Object ? parent->members[j].key_start
					: parent->items[j].start;
		};
		auto end_of = [&](size_t j) {
			return parent->type == Type::Object ? parent->members[j].node.end
					: parent->items[j].end;
		};
		size_t s = start_of(i), e = end_of(i);
		size_t comma = comma_after(text_, e);
		if (comma != std::string::npos) {
			e = comma + 1;
		} else if (i > 0) {
			s = end_of(i - 1);  /* the comma before it goes */
		}
		bool whole_line = false;
		/* A line of its own: the line goes. */
		if (starts_line(text_, s)) {
			size_t eol = quiet_line_end(text_, e);
			if (eol != std::string::npos) {
				bool comment = false;
				for (size_t j = e; j < eol; ++j)
					if (text_[j] == '/')
						comment = true;
				if (!comment) {
					size_t ls = s;
					while (ls > 0 && text_[ls - 1] != '\n')
						--ls;
					s = ls;
					e = eol < text_.size() ? eol + 1 : eol;
					whole_line = true;
				}
			}
		}
		/* On a line with others: the space after its comma goes too. */
		if (!whole_line && comma != std::string::npos)
			while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t'))
				++e;
		edit(s, e - s, "");
		reparse();
		if (!ok())
			return false;
		(void)count;
	}
	return true;
}

}  // namespace wb::jsonc
