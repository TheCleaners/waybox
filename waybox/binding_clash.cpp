#include "waybox/binding_clash.hpp"

#include <algorithm>
#include <cctype>

#include "waybox/key_label.hpp"

namespace wb {

std::string canonical_key(std::string_view key) {
	std::string rest;
	HeldModifiers held = held_of(key, rest);
	std::string out = rc_key(held, rest);
	return out.empty() ? std::string(key) : out;  /* a lone modifier, as is */
}

namespace {

/* A Lua long bracket at `at`: "[[", "[=[", "[==[" ...; its level, or -1. */
int long_bracket(std::string_view s, size_t at) {
	if (at >= s.size() || s[at] != '[')
		return -1;
	size_t p = at + 1;
	int level = 0;
	while (p < s.size() && s[p] == '=') {
		++level;
		++p;
	}
	return p < s.size() && s[p] == '[' ? level : -1;
}

bool ident_char(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

std::vector<ScriptKey> script_keys(std::string_view lua, const std::string &file) {
	std::vector<ScriptKey> out;
	int line = 1;
	size_t at = 0;
	auto count_lines = [&](size_t from, size_t to) {
		for (size_t i = from; i < to && i < lua.size(); ++i)
			if (lua[i] == '\n')
				++line;
	};
	/* Skips past a long bracket's close ("]==]"); the end, or npos. */
	auto skip_long = [&](size_t open, int level) {
		std::string close = "]" + std::string(static_cast<size_t>(level), '=') + "]";
		size_t end = lua.find(close, open);
		size_t to = end == std::string_view::npos ? lua.size() : end + close.size();
		count_lines(open, to);
		return to;
	};
	/* A quoted string from `at` (the quote): past its end. */
	auto skip_quoted = [&](size_t q) {
		char quote = lua[q];
		size_t p = q + 1;
		while (p < lua.size() && lua[p] != quote && lua[p] != '\n') {
			if (lua[p] == '\\')
				++p;
			++p;
		}
		return p < lua.size() ? p + 1 : p;
	};
	while (at < lua.size()) {
		char c = lua[at];
		if (c == '\n') {
			++line;
			++at;
		} else if (lua.compare(at, 2, "--") == 0) {
			/* A comment: long (--[[ ... ]], over lines) or to the line's end. */
			int level = long_bracket(lua, at + 2);
			if (level >= 0) {
				at = skip_long(at + 2, level);
			} else {
				size_t nl = lua.find('\n', at);
				at = nl == std::string_view::npos ? lua.size() : nl;
			}
		} else if (c == '"' || c == '\'') {
			at = skip_quoted(at);
		} else if (int level = long_bracket(lua, at); level >= 0) {
			at = skip_long(at, level);  /* a long string */
		} else if (lua.compare(at, 7, "wb.bind") == 0 && (at == 0 || !ident_char(lua[at - 1])) &&
				(at + 7 >= lua.size() || !ident_char(lua[at + 7]))) {
			size_t p = at + 7;
			while (p < lua.size() && (lua[p] == ' ' || lua[p] == '\t'))
				++p;
			if (p < lua.size() && lua[p] == '(')
				++p;
			while (p < lua.size() && (lua[p] == ' ' || lua[p] == '\t'))
				++p;
			if (p < lua.size() && (lua[p] == '"' || lua[p] == '\'')) {
				size_t end = skip_quoted(p);
				if (end > p + 1 && lua[end - 1] == lua[p])
					out.push_back({std::string(lua.substr(p + 1, end - p - 2)), file, line});
				at = end;
			} else {
				at += 7;
			}
		} else if (ident_char(c)) {
			/* A whole name: "mywb.bind" isn't wb.bind. */
			while (at < lua.size() && (ident_char(lua[at]) || lua[at] == '.'))
				++at;
		} else {
			++at;
		}
	}
	return out;
}

namespace {

std::vector<std::string> canonical(const std::vector<std::string> &chain) {
	std::vector<std::string> out;
	for (const std::string &k : chain)
		out.push_back(canonical_key(k));
	return out;
}

}  // namespace

std::vector<std::optional<Clash>> key_clashes(const std::vector<std::vector<std::string>> &chains,
		const std::vector<ScriptKey> &scripts) {
	std::vector<std::vector<std::string>> keys;
	for (const auto &c : chains)
		keys.push_back(canonical(c));
	std::vector<std::optional<Clash>> out(chains.size());
	for (size_t i = 0; i < keys.size(); ++i) {
		const auto &k = keys[i];
		if (k.empty())
			continue;
		/* Scripts first: they take a first key outright. */
		for (const ScriptKey &s : scripts)
			if (canonical_key(s.key) == k[0]) {
				Clash c;
				c.script = s;
				c.keys = {chains[i][0]};
				out[i] = c;
				break;
			}
		if (out[i])
			continue;
		for (size_t j = 0; j < i; ++j) {
			const auto &e = keys[j];
			if (e.empty())
				continue;
			size_t n = std::min(e.size(), k.size());
			if (!std::equal(e.begin(), e.begin() + static_cast<long>(n), k.begin())) {
				/* Different keys within the shorter one: apart, unless a
				 * shared start is spelled differently ("S-W-a", "W-S-a"):
				 * then they're two <keybind>s, and Waybox only goes into
				 * the first; the later chain never runs from there on. */
				size_t d = 0;
				while (d < n && e[d] == k[d] && chains[j][d] == chains[i][d])
					++d;
				if (d < n && e[d] == k[d]) {
					Clash c;
					c.by = j;
					c.keys.assign(chains[i].begin(), chains[i].begin() + static_cast<long>(d + 1));
					c.by_chain = e.size() > d + 1;
					out[i] = c;
					break;
				}
				continue;
			}
			/* Same keys, or one the start of the other: the earlier takes
			 * them. (Chains that only share a start, "C-a x" and "C-a y",
			 * differ within the shorter one: no clash.) */
			Clash c;
			c.by = j;
			c.keys.assign(chains[i].begin(), chains[i].begin() + static_cast<long>(n));
			c.by_chain = e.size() > k.size();
			out[i] = c;
			break;
		}
	}
	return out;
}

std::vector<std::optional<size_t>> mouse_clashes(const std::vector<MousePlace> &places) {
	std::vector<std::optional<size_t>> out(places.size());
	for (size_t i = 0; i < places.size(); ++i)
		for (size_t j = 0; j < i; ++j)
			if (places[j].context == places[i].context && places[j].event == places[i].event &&
					canonical_key(places[j].button) == canonical_key(places[i].button)) {
				out[i] = j;
				break;
			}
	return out;
}

}  // namespace wb
