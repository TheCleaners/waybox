#ifndef WB_BINDING_CLASH_HPP
#define WB_BINDING_CLASH_HPP

/*
 * Which bindings never run, as Waybox picks: scripts' keys (wb.bind) come
 * before rc.xml's; within rc.xml, at each step of a chain the first
 * binding on a key wins (keys compared as pressed: "W-D", "W-d" and
 * "S-W-d" vs "W-S-d" alike). So a binding never runs when an earlier one
 * has the same keys, or when one's keys begin the other's ("C-a" alone
 * and "C-a then x": the first of the two takes C-a). Pure
 * (test/binding_clash_test.cpp).
 */
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/* A key as pressed: modifiers in one order, a letter lowercase. */
std::string canonical_key(std::string_view key);

/* A key a script binds: wb.bind("W-x", ...) in `file` at `line`. */
struct ScriptKey {
	std::string key, file;
	int line = 0;
};

/* The literal wb.bind("...") calls in a Lua script's text (comments
 * skipped); keys built at run time can't be seen. */
std::vector<ScriptKey> script_keys(std::string_view lua, const std::string &file);
/* The same for any call: `name`("first argument", ...) ("wb.policy" ->
 * "rules", "place"...), its first argument as `key`. */
std::vector<ScriptKey> script_calls(std::string_view lua, const std::string &file,
		std::string_view name);

/* Why a binding never runs: the binding (index into what was given) or
 * script that takes its keys first, and the keys taken ("W-a", or the
 * shared start of two chains). */
struct Clash {
	std::optional<size_t> by;          /* a binding, earlier */
	std::optional<ScriptKey> script;   /* or a script */
	std::vector<std::string> keys;
	bool by_chain = false;             /* `by` is a chain these keys start */
};

/* For each binding, in rc.xml's order (empty chains: not counted), why it
 * never runs, if it doesn't. */
std::vector<std::optional<Clash>> key_clashes(const std::vector<std::vector<std::string>> &chains,
		const std::vector<ScriptKey> &scripts);

/* Mouse bindings: the same place, button (as pressed) and event as an
 * earlier one. */
struct MousePlace {
	std::string context, button, event;
};
std::vector<std::optional<size_t>> mouse_clashes(const std::vector<MousePlace> &places);

}  // namespace wb

#endif /* WB_BINDING_CLASH_HPP */
