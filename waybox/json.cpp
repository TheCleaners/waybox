/*
 * JSON for the IPC socket (see json.hpp).
 */
#include "waybox/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wb::json {

const Value *Value::get(const std::string &key) const {
	const Object *o = object();
	if (o == nullptr)
		return nullptr;
	auto it = o->find(key);
	return it == o->end() ? nullptr : &it->second;
}

std::string Value::str(const std::string &key, const std::string &fallback) const {
	const Value *m = get(key);
	if (m == nullptr)
		return fallback;
	if (const std::string *s = m->string())
		return *s;
	if (const double *d = m->number()) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%.15g", *d);
		return buf;
	}
	if (const bool *b = m->boolean())
		return *b ? "true" : "false";
	return fallback;
}

namespace {

struct Parser {
	std::string_view s;
	size_t i = 0;
	int depth = 0;

	void ws() {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
			++i;
	}
	bool lit(std::string_view w) {
		if (s.substr(i, w.size()) != w)
			return false;
		i += w.size();
		return true;
	}
	static void utf8(std::string &out, unsigned cp) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800) {
			out += static_cast<char>(0xc0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			out += static_cast<char>(0xe0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		} else {
			out += static_cast<char>(0xf0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (cp & 0x3f));
		}
	}
	bool hex4(unsigned &cp) {
		if (i + 4 > s.size())
			return false;
		cp = 0;
		for (int k = 0; k < 4; ++k) {
			char c = s[i++];
			cp <<= 4;
			if (c >= '0' && c <= '9') cp |= c - '0';
			else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
			else return false;
		}
		return true;
	}
	std::optional<std::string> string() {
		if (i >= s.size() || s[i] != '"')
			return std::nullopt;
		++i;
		std::string out;
		while (i < s.size()) {
			char c = s[i++];
			if (c == '"')
				return out;
			if (static_cast<unsigned char>(c) < 0x20)
				return std::nullopt;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i >= s.size())
				return std::nullopt;
			char e = s[i++];
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
				unsigned cp;
				if (!hex4(cp))
					return std::nullopt;
				if (cp >= 0xd800 && cp < 0xdc00) {  /* a surrogate pair */
					unsigned lo;
					if (!lit("\\u") || !hex4(lo) || lo < 0xdc00 || lo >= 0xe000)
						return std::nullopt;
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				}
				utf8(out, cp);
				break;
			}
			default:
				return std::nullopt;
			}
		}
		return std::nullopt;
	}
	std::optional<Value> value() {
		ws();
		if (i >= s.size() || depth > 64)
			return std::nullopt;
		char c = s[i];
		if (c == 'n') return lit("null") ? std::optional<Value>(Value()) : std::nullopt;
		if (c == 't') return lit("true") ? std::optional<Value>(Value(true)) : std::nullopt;
		if (c == 'f') return lit("false") ? std::optional<Value>(Value(false)) : std::nullopt;
		if (c == '"') {
			auto str = string();
			if (!str)
				return std::nullopt;
			return Value(std::move(*str));
		}
		if (c == '[') {
			++i;
			++depth;
			Array a;
			ws();
			if (i < s.size() && s[i] == ']') {
				++i;
				--depth;
				return Value(std::move(a));
			}
			while (true) {
				auto v = value();
				if (!v)
					return std::nullopt;
				a.push_back(std::move(*v));
				ws();
				if (i < s.size() && s[i] == ',') { ++i; continue; }
				if (i < s.size() && s[i] == ']') { ++i; --depth; return Value(std::move(a)); }
				return std::nullopt;
			}
		}
		if (c == '{') {
			++i;
			++depth;
			Object o;
			ws();
			if (i < s.size() && s[i] == '}') {
				++i;
				--depth;
				return Value(std::move(o));
			}
			while (true) {
				ws();
				auto key = string();
				if (!key)
					return std::nullopt;
				ws();
				if (i >= s.size() || s[i] != ':')
					return std::nullopt;
				++i;
				auto v = value();
				if (!v)
					return std::nullopt;
				o[*key] = std::move(*v);
				ws();
				if (i < s.size() && s[i] == ',') { ++i; continue; }
				if (i < s.size() && s[i] == '}') { ++i; --depth; return Value(std::move(o)); }
				return std::nullopt;
			}
		}
		/* A number. */
		size_t start = i;
		if (i < s.size() && s[i] == '-') ++i;
		while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
				s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
			++i;
		if (i == start)
			return std::nullopt;
		std::string num(s.substr(start, i - start));
		char *end = nullptr;
		double d = strtod(num.c_str(), &end);
		if (end == nullptr || *end != '\0' || !std::isfinite(d))
			return std::nullopt;
		return Value(d);
	}
};

void dump_string(std::string &out, const std::string &s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

void dump_to(std::string &out, const Value &v) {
	if (v.is_null()) {
		out += "null";
	} else if (const bool *b = v.boolean()) {
		out += *b ? "true" : "false";
	} else if (const double *d = v.number()) {
		char buf[32];
		if (*d == std::floor(*d) && std::fabs(*d) < 1e15)
			snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*d));
		else
			snprintf(buf, sizeof(buf), "%.15g", *d);
		out += buf;
	} else if (const std::string *s = v.string()) {
		dump_string(out, *s);
	} else if (const Array *a = v.array()) {
		out += '[';
		for (size_t k = 0; k < a->size(); ++k) {
			if (k)
				out += ',';
			dump_to(out, (*a)[k]);
		}
		out += ']';
	} else if (const Object *o = v.object()) {
		out += '{';
		bool first = true;
		for (const auto &[key, val] : *o) {
			if (!first)
				out += ',';
			first = false;
			dump_string(out, key);
			out += ':';
			dump_to(out, val);
		}
		out += '}';
	}
}

}  // namespace

std::optional<Value> parse(std::string_view text) {
	Parser p{text};
	auto v = p.value();
	if (!v)
		return std::nullopt;
	p.ws();
	if (p.i != text.size())
		return std::nullopt;
	return v;
}

std::string dump(const Value &value) {
	std::string out;
	dump_to(out, value);
	return out;
}

}  // namespace wb::json
