#include "waybox/binding_clash.hpp"

#include <algorithm>

#include "waybox/key_label.hpp"

namespace wb {

std::string canonical_key(std::string_view key) {
	std::string rest;
	HeldModifiers held = held_of(key, rest);
	std::string out = rc_key(held, rest);
	return out.empty() ? std::string(key) : out;  /* a lone modifier, as is */
}

std::vector<ScriptKey> script_keys(std::string_view lua, const std::string &file) {
	std::vector<ScriptKey> out;
	int line = 1;
	size_t at = 0;
	bool in_comment = false;  /* a line comment, to its end */
	while (at < lua.size()) {
		char c = lua[at];
		if (c == '\n') {
			++line;
			in_comment = false;
			++at;
			continue;
		}
		if (in_comment) {
			++at;
			continue;
		}
		if (lua.compare(at, 2, "--") == 0) {
			in_comment = true;
			at += 2;
			continue;
		}
		if (lua.compare(at, 7, "wb.bind") == 0) {
			size_t p = at + 7;
			while (p < lua.size() && (lua[p] == ' ' || lua[p] == '\t'))
				++p;
			if (p < lua.size() && lua[p] == '(')
				++p;
			while (p < lua.size() && (lua[p] == ' ' || lua[p] == '\t'))
				++p;
			if (p < lua.size() && (lua[p] == '"' || lua[p] == '\'')) {
				char q = lua[p];
				size_t end = lua.find(q, p + 1);
				if (end != std::string_view::npos && lua.substr(p + 1, end - p - 1).find('\n') ==
						std::string_view::npos)
					out.push_back({std::string(lua.substr(p + 1, end - p - 1)), file, line});
			}
			at += 7;
			continue;
		}
		++at;
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
			if (!std::equal(e.begin(), e.begin() + static_cast<long>(n), k.begin()))
				continue;
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
