/*
 * wbconf-lua: runs an integration's settings page written in Lua
 * (Page=NAME.lua) for wbconf, in a process of its own, speaking the page
 * protocol (waybox/page_protocol.hpp) on stdin and stdout.
 *
 * The page runs in a sandbox: no io, os, package, debug, load or require;
 * reading only the files its integration lists in Files=, writing and
 * running only as requests that wbconf carries out on save (and checks
 * again). Each call has a time budget (an instruction-count hook) and the
 * process a memory limit, so a broken or slow page shows as unavailable
 * rather than taking wbconf with it. Globals are strict: assigning or
 * reading an undeclared one is an error, said with its line.
 */
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <lua.hpp>

#include "waybox/json.hpp"
#include "waybox/ini_doc.hpp"
#include "waybox/jsonc_doc.hpp"
#include "waybox/toml_doc.hpp"
#include "waybox/page_protocol.hpp"

#if LUA_VERSION_NUM < 502  /* LuaJIT, Lua 5.1 */
#define lua_rawlen lua_objlen
static int lua_absindex(lua_State *L, int i) {
	return i > 0 || i <= LUA_REGISTRYINDEX ? i : lua_gettop(L) + i + 1;
}
static const char *luaL_tolstring(lua_State *L, int i, size_t *len) {
	i = lua_absindex(L, i);
	if (lua_isstring(L, i))
		lua_pushvalue(L, i);
	else if (lua_isnil(L, i))
		lua_pushliteral(L, "nil");
	else if (lua_isboolean(L, i))
		lua_pushstring(L, lua_toboolean(L, i) ? "true" : "false");
	else
		lua_pushstring(L, lua_typename(L, lua_type(L, i)));
	return lua_tolstring(L, -1, len);
}
#endif

namespace json = wb::json;

namespace {

constexpr int kCallMs = 250;         /* each call into the page */
constexpr int kOpenMs = 1000;        /* loading it (reading its files) */
constexpr rlim_t kMemory = 512u << 20;  /* the process's address space */
constexpr size_t kMaxRead = 1u << 20;   /* wb.read: a config file, not a disk */

struct Runner {
	lua_State *L = nullptr;
	std::vector<std::string> files, runs;
	std::map<std::string, std::string> options;  /* its command's, by id */
	std::string home, script_name;
	std::vector<json::Value> out;  /* this call's messages */
	long long deadline = 0;
	int api_ref = LUA_NOREF;       /* the prelude's internals */
} R;

long long now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

void emit(json::Value v) {
	R.out.push_back(std::move(v));
}

void flush(bool done = true) {
	for (const json::Value &v : R.out)
		std::cout << json::dump(v) << "\n";
	R.out.clear();
	if (done)
		std::cout << json::dump(json::Object{{"done", true}}) << "\n";
	std::cout.flush();
}

void hook(lua_State *L, lua_Debug *) {
	if (now_ms() > R.deadline)
		luaL_error(L, "took longer than %d ms: stopped", kCallMs);
}

/* "page.lua:14: message" -> line 14, "message". */
void emit_error(const std::string &raw) {
	int line = 0;
	std::string msg = raw;
	std::string prefix = R.script_name + ":";
	if (raw.rfind(prefix, 0) == 0) {
		size_t colon = raw.find(':', prefix.size());
		std::string num = raw.substr(prefix.size(), colon == std::string::npos ? 0
				: colon - prefix.size());
		if (!num.empty() && std::all_of(num.begin(), num.end(), ::isdigit)) {
			line = std::atoi(num.c_str());
			msg = raw.substr(colon + 1);
			while (!msg.empty() && msg[0] == ' ')
				msg.erase(0, 1);
		}
	}
	emit(json::Object{{"error", json::Object{{"line", line}, {"message", msg}}}});
}

/* Call the function on the stack (with `nargs` under it) within `ms`. */
bool call(int nargs, int ms) {
	R.deadline = now_ms() + ms;
	if (lua_pcall(R.L, nargs, 0, 0) != 0) {
		const char *e = lua_tostring(R.L, -1);
		emit_error(e != nullptr ? e : "error");
		lua_pop(R.L, 1);
		return false;
	}
	return true;
}

/* ---- Lua values to JSON ---- */

json::Value to_json(lua_State *L, int idx, int depth) {
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
	case LUA_TBOOLEAN:
		return json::Value(lua_toboolean(L, idx) != 0);
	case LUA_TNUMBER:
		return json::Value(static_cast<double>(lua_tonumber(L, idx)));
	case LUA_TSTRING:
		return json::Value(std::string(lua_tostring(L, idx)));
	case LUA_TTABLE: {
		if (depth > 16)
			return json::Value();
		size_t n = lua_rawlen(L, idx);
		if (n > 0) {
			json::Array a;
			for (size_t i = 1; i <= n; ++i) {
				lua_rawgeti(L, idx, static_cast<int>(i));
				a.push_back(to_json(L, -1, depth + 1));
				lua_pop(L, 1);
			}
			return a;
		}
		json::Object o;
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TFUNCTION)
				o[lua_tostring(L, -2)] = to_json(L, -1, depth + 1);
			lua_pop(L, 1);
		}
		return o;
	}
	default:
		return json::Value();
	}
}

void push_json(lua_State *L, const json::Value &v) {
	if (const std::string *s = v.string())
		lua_pushlstring(L, s->data(), s->size());
	else if (const double *d = v.number())
		lua_pushnumber(L, *d);
	else if (const bool *b = v.boolean())
		lua_pushboolean(L, *b);
	else if (const json::Object *o = v.object()) {
		lua_newtable(L);
		for (const auto &[k, val] : *o) {
			push_json(L, val);
			lua_setfield(L, -2, k.c_str());
		}
	} else if (const json::Array *a = v.array()) {
		lua_newtable(L);
		int i = 1;
		for (const json::Value &e : *a) {
			push_json(L, e);
			lua_rawseti(L, -2, i++);
		}
	} else {
		lua_pushnil(L);
	}
}

/* ---- wb.* in C ---- */

std::vector<std::string> string_list(lua_State *L, int idx, const char *what) {
	luaL_checktype(L, idx, LUA_TTABLE);
	std::vector<std::string> out;
	size_t n = lua_rawlen(L, idx);
	for (size_t i = 1; i <= n; ++i) {
		lua_rawgeti(L, idx, static_cast<int>(i));
		if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER)
			luaL_error(L, "%s: word %d isn't text", what, static_cast<int>(i));
		out.push_back(lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	return out;
}

/* wb.read(path): a file's text, if it's one of Files=; nil and why if
 * it can't be read (a page usually starts from defaults then). */
int wb_read(lua_State *L) {
	std::string path = luaL_checkstring(L, 1);
	if (!wb::page::path_allowed(path, R.files, R.home))
		return luaL_error(L, "wb.read: %s isn't in the integration's Files=", path.c_str());
	std::ifstream f(wb::page::expand_home(path, R.home), std::ios::binary);
	if (!f) {
		lua_pushnil(L);
		lua_pushstring(L, "can't be read (missing?)");
		return 2;
	}
	std::string text(kMaxRead + 1, '\0');
	f.read(text.data(), static_cast<std::streamsize>(text.size()));
	text.resize(static_cast<size_t>(f.gcount()));
	if (text.size() > kMaxRead)
		return luaL_error(L, "wb.read: %s is over 1 MB", path.c_str());
	lua_pushlstring(L, text.data(), text.size());
	return 1;
}

/* wb.write(path, text): written by wbconf on save (a backup kept). */
int wb_write(lua_State *L) {
	std::string path = luaL_checkstring(L, 1);
	size_t len = 0;
	const char *text = luaL_checklstring(L, 2, &len);
	if (!wb::page::path_allowed(path, R.files, R.home))
		return luaL_error(L, "wb.write: %s isn't in the integration's Files=", path.c_str());
	emit(json::Object{{"write", json::Object{{"path", wb::page::expand_home(path, R.home)},
			{"text", std::string(text, len)}}}});
	return 0;
}

/* wb.run { "program", "arg", ... }: run by wbconf (after a save's
 * writes); the program one of Runs=. */
int wb_run(lua_State *L) {
	std::vector<std::string> argv = string_list(L, 1, "wb.run");
	if (!wb::page::run_allowed(argv, R.runs))
		return luaL_error(L, "wb.run: %s isn't in the integration's Runs=",
				argv.empty() ? "(nothing)" : argv[0].c_str());
	json::Array a;
	for (const std::string &s : argv)
		a.emplace_back(s);
	emit(json::Object{{"run", std::move(a)}});
	return 0;
}

/* wb.reload(): the program takes its settings again, as its integration's
 * Reload= says (wbconf does it, after the save's writes). */
/* wb.option(id): the integration's option as the program's command sets it
 * (waybar's -c), nil when it doesn't. */
int wb_option(lua_State *L) {
	auto it = R.options.find(luaL_checkstring(L, 1));
	if (it == R.options.end() || it->second.empty())
		lua_pushnil(L);
	else
		lua_pushlstring(L, it->second.data(), it->second.size());
	return 1;
}

int wb_reload(lua_State *) {
	emit(json::Object{{"reload", true}});
	return 0;
}

/* The prelude's wb.set calls this once the key is checked. */
int wb_update(lua_State *L) {
	std::string key = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	emit(json::Object{{"update", json::Object{{key, to_json(L, 2, 0)}}}});
	return 0;
}

int wb_log(lua_State *L) {
	int n = lua_gettop(L);
	std::string line;
	for (int i = 1; i <= n; ++i) {
		if (i > 1)
			line += " ";
		const char *s = luaL_tolstring(L, i, nullptr);
		line += s != nullptr ? s : "";
		lua_pop(L, 1);
	}
	fprintf(stderr, "%s: %s\n", R.script_name.c_str(), line.c_str());
	return 0;
}

/* ---- wb.ini's document, in C (ini_doc.cpp) ---- */

const char *kIniMeta = "wb.ini.document";

wb::ini::Document *ini_doc(lua_State *L) {
	return static_cast<wb::ini::Document *>(luaL_checkudata(L, 1, kIniMeta));
}

int ini_gc(lua_State *L) {
	ini_doc(L)->~Document();
	return 0;
}

int ini_parse(lua_State *L) {
	size_t len = 0;
	const char *text = luaL_optlstring(L, 1, "", &len);
	void *mem = lua_newuserdata(L, sizeof(wb::ini::Document));
	new (mem) wb::ini::Document(wb::ini::Document::parse(std::string_view(text, len)));
	luaL_getmetatable(L, kIniMeta);
	lua_setmetatable(L, -2);
	return 1;
}

int ini_get(lua_State *L) {
	auto v = ini_doc(L)->get(luaL_checkstring(L, 2), luaL_checkstring(L, 3));
	if (v)
		lua_pushlstring(L, v->data(), v->size());
	else
		lua_pushnil(L);
	return 1;
}

int ini_set(lua_State *L) {
	ini_doc(L)->set(luaL_checkstring(L, 2), luaL_checkstring(L, 3), luaL_checkstring(L, 4));
	return 0;
}

int ini_remove(lua_State *L) {
	lua_pushboolean(L, ini_doc(L)->remove(luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
	return 1;
}

int ini_sections(lua_State *L) {
	auto s = ini_doc(L)->sections();
	lua_newtable(L);
	for (size_t i = 0; i < s.size(); ++i) {
		lua_pushstring(L, s[i].c_str());
		lua_rawseti(L, -2, static_cast<int>(i + 1));
	}
	return 1;
}

int ini_entries(lua_State *L) {
	auto e = ini_doc(L)->entries(luaL_checkstring(L, 2));
	lua_newtable(L);
	for (size_t i = 0; i < e.size(); ++i) {
		lua_newtable(L);
		lua_pushstring(L, e[i].first.c_str());
		lua_setfield(L, -2, "key");
		lua_pushstring(L, e[i].second.c_str());
		lua_setfield(L, -2, "value");
		lua_rawseti(L, -2, static_cast<int>(i + 1));
	}
	return 1;
}

int ini_remove_section(lua_State *L) {
	lua_pushboolean(L, ini_doc(L)->remove_section(luaL_checkstring(L, 2)));
	return 1;
}

int ini_move_section_to_end(lua_State *L) {
	lua_pushboolean(L, ini_doc(L)->move_section_to_end(luaL_checkstring(L, 2)));
	return 1;
}

int ini_text(lua_State *L) {
	std::string t = ini_doc(L)->text();
	lua_pushlstring(L, t.data(), t.size());
	return 1;
}

/* The C side of wb.ini, handed to the prelude. */
void push_ini_api(lua_State *L) {
	luaL_newmetatable(L, kIniMeta);
	lua_pushcfunction(L, ini_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	lua_newtable(L);
	const luaL_Reg fns[] = {{"parse", ini_parse}, {"get", ini_get}, {"set", ini_set},
			{"remove", ini_remove}, {"sections", ini_sections}, {"entries", ini_entries},
			{"text", ini_text}, {"remove_section", ini_remove_section},
			{"move_section_to_end", ini_move_section_to_end}, {nullptr, nullptr}};
	for (const luaL_Reg *f = fns; f->name != nullptr; ++f) {
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
}

/* ---- wb.json's document, in C (jsonc_doc.cpp) ---- */

const char *kJsonMeta = "wb.json.document";
const char *kJsonArrayMeta = "wb.json.array";
int json_null_tag;  /* wb.json.null: its address */

wb::jsonc::Document *json_doc(lua_State *L) {
	return static_cast<wb::jsonc::Document *>(luaL_checkudata(L, 1, kJsonMeta));
}

int json_gc(lua_State *L) {
	json_doc(L)->~Document();
	return 0;
}

/* parse(text) -> document | nil, "line 3, column 5: ..." */
int json_parse(lua_State *L) {
	size_t len = 0;
	const char *text = luaL_optlstring(L, 1, "", &len);
	auto d = wb::jsonc::Document::parse(std::string_view(text, len));
	if (!d.ok()) {
		lua_pushnil(L);
		lua_pushstring(L, d.error().c_str());
		return 2;
	}
	void *mem = lua_newuserdata(L, sizeof(wb::jsonc::Document));
	new (mem) wb::jsonc::Document(std::move(d));
	luaL_getmetatable(L, kJsonMeta);
	lua_setmetatable(L, -2);
	return 1;
}

/* A path: "key", 1 (an index, from 1), or { "key", 1, ... }. */
wb::jsonc::Path json_path(lua_State *L, int at) {
	wb::jsonc::Path path;
	auto part = [L, &path](int i) {
		if (lua_type(L, i) == LUA_TSTRING) {
			size_t n = 0;
			const char *s = lua_tolstring(L, i, &n);
			path.emplace_back(std::string(s, n));
		} else if (lua_type(L, i) == LUA_TNUMBER) {
			lua_Number n = lua_tonumber(L, i);
			if (n < 1 || n != std::floor(n) || n > 1e9)
				luaL_error(L, "a path's index counts from 1");
			path.emplace_back(static_cast<size_t>(n) - 1);
		} else {
			luaL_error(L, "a path is a key, an index or a table of them");
		}
	};
	if (lua_type(L, at) == LUA_TTABLE) {
		size_t n = lua_rawlen(L, at);
		for (size_t i = 1; i <= n; ++i) {
			lua_rawgeti(L, at, static_cast<int>(i));
			part(lua_gettop(L));
			lua_pop(L, 1);
		}
	} else if (!lua_isnoneornil(L, at)) {
		part(at);
	}
	return path;
}

void push_json(lua_State *L, const wb::jsonc::Value &v) {
	using T = wb::jsonc::Value::Type;
	luaL_checkstack(L, 4, "nested too deeply");
	switch (v.type) {
	case T::Null: lua_pushlightuserdata(L, &json_null_tag); break;
	case T::Bool: lua_pushboolean(L, v.boolean); break;
	case T::Integer: lua_pushnumber(L, static_cast<lua_Number>(v.integer)); break;
	case T::Number: lua_pushnumber(L, v.number); break;
	case T::String: lua_pushlstring(L, v.string.data(), v.string.size()); break;
	case T::Array:
		lua_createtable(L, static_cast<int>(v.items.size()), 0);
		for (size_t i = 0; i < v.items.size(); ++i) {
			push_json(L, v.items[i]);
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
		luaL_getmetatable(L, kJsonArrayMeta);
		lua_setmetatable(L, -2);
		break;
	case T::Object:
		lua_createtable(L, 0, static_cast<int>(v.members.size()));
		for (const auto &m : v.members) {
			lua_pushlstring(L, m.first.data(), m.first.size());
			push_json(L, m.second);
			lua_rawset(L, -3);
		}
		break;
	}
}

/* A Lua value as JSON: a table with 1..n (or marked wb.json.array) is an
 * array, any other an object (keys sorted: the file doesn't change with
 * Lua's table order). */
wb::jsonc::Value lua_to_jsonc(lua_State *L, int at, int depth) {
	using T = wb::jsonc::Value::Type;
	wb::jsonc::Value v;
	if (depth > 64)
		luaL_error(L, "a value nested too deeply (or a table inside itself)");
	at = at < 0 ? lua_gettop(L) + at + 1 : at;
	switch (lua_type(L, at)) {
	case LUA_TNIL: break;
	case LUA_TLIGHTUSERDATA:
		if (lua_touserdata(L, at) != &json_null_tag)
			luaL_error(L, "a value JSON can't hold");
		break;
	case LUA_TBOOLEAN: v.type = T::Bool; v.boolean = lua_toboolean(L, at); break;
	case LUA_TNUMBER: {
		lua_Number n = lua_tonumber(L, at);
		if (n == std::floor(n) && std::fabs(n) < 9007199254740992.0) {
			v.type = T::Integer;
			v.integer = static_cast<int64_t>(n);
		} else {
			v.type = T::Number;
			v.number = n;
		}
		break;
	}
	case LUA_TSTRING: {
		size_t len = 0;
		const char *s = lua_tolstring(L, at, &len);
		v.type = T::String;
		v.string.assign(s, len);
		break;
	}
	case LUA_TTABLE: {
		luaL_checkstack(L, 6, "nested too deeply");
		size_t n = lua_rawlen(L, at);
		bool marked = false;
		if (lua_getmetatable(L, at)) {
			luaL_getmetatable(L, kJsonArrayMeta);
			marked = lua_rawequal(L, -1, -2);
			lua_pop(L, 2);
		}
		size_t keys = 0;
		bool sequence = true;
		std::vector<std::string> names;
		lua_pushnil(L);
		while (lua_next(L, at) != 0) {
			++keys;
			if (lua_type(L, -2) == LUA_TSTRING) {
				sequence = false;
				names.emplace_back(lua_tostring(L, -2));
			} else if (lua_type(L, -2) != LUA_TNUMBER) {
				lua_pop(L, 2);
				luaL_error(L, "a table's keys are strings (an object) or 1..n (an array)");
			}
			lua_pop(L, 1);
		}
		if (marked || (sequence && keys == n && n > 0)) {
			if (!sequence)
				luaL_error(L, "an array (wb.json.array) with string keys");
			v.type = T::Array;
			for (size_t i = 1; i <= n; ++i) {
				lua_rawgeti(L, at, static_cast<int>(i));
				v.items.push_back(lua_to_jsonc(L, -1, depth + 1));
				lua_pop(L, 1);
			}
		} else {
			if (keys != names.size())
				luaL_error(L, "a table with both string keys and numbered ones");
			v.type = T::Object;
			std::sort(names.begin(), names.end());
			for (const std::string &k : names) {
				lua_pushlstring(L, k.data(), k.size());
				lua_rawget(L, at);
				v.members.emplace_back(k, lua_to_jsonc(L, -1, depth + 1));
				lua_pop(L, 1);
			}
		}
		break;
	}
	default:
		luaL_error(L, "a %s can't be written as JSON", luaL_typename(L, at));
	}
	return v;
}

int json_get(lua_State *L) {
	auto v = json_doc(L)->get(json_path(L, 2));
	if (v)
		push_json(L, *v);
	else
		lua_pushnil(L);
	return 1;
}

int json_set(lua_State *L) {
	auto *d = json_doc(L);
	wb::jsonc::Path path = json_path(L, 2);
	wb::jsonc::Value v = lua_to_jsonc(L, 3, 0);
	lua_pushboolean(L, d->set(path, v));
	return 1;
}

int json_remove(lua_State *L) {
	lua_pushboolean(L, json_doc(L)->remove(json_path(L, 2)));
	return 1;
}

/* An object's keys, in the file's order. */
int json_keys(lua_State *L) {
	auto v = json_doc(L)->get(json_path(L, 2));
	lua_newtable(L);
	if (v && v->type == wb::jsonc::Value::Type::Object)
		for (size_t i = 0; i < v->members.size(); ++i) {
			lua_pushlstring(L, v->members[i].first.data(), v->members[i].first.size());
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
	return 1;
}

int json_text(lua_State *L) {
	const std::string &t = json_doc(L)->text();
	lua_pushlstring(L, t.data(), t.size());
	return 1;
}

/* A value as the page protocol's JSON text, and back (sections' values). */
int json_wire_encode(lua_State *L) {
	std::string t = json::dump(to_json(L, 1, 0));
	lua_pushlstring(L, t.data(), t.size());
	return 1;
}

int json_wire_decode(lua_State *L) {
	auto v = json::parse(luaL_optstring(L, 1, ""));
	if (v)
		push_json(L, *v);
	else
		lua_newtable(L);
	return 1;
}

/* The C side of wb.json, handed to the prelude. */
void push_json_api(lua_State *L) {
	luaL_newmetatable(L, kJsonMeta);
	lua_pushcfunction(L, json_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_newmetatable(L, kJsonArrayMeta);
	lua_pop(L, 1);
	lua_newtable(L);
	const luaL_Reg fns[] = {{"parse", json_parse}, {"get", json_get}, {"set", json_set},
			{"remove", json_remove}, {"keys", json_keys}, {"text", json_text},
			{"wire_encode", json_wire_encode}, {"wire_decode", json_wire_decode},
			{nullptr, nullptr}};
	for (const luaL_Reg *f = fns; f->name != nullptr; ++f) {
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_pushlightuserdata(L, &json_null_tag);
	lua_setfield(L, -2, "null");
	luaL_getmetatable(L, kJsonArrayMeta);
	lua_setfield(L, -2, "array_mt");
}

/* ---- wb.toml's document, in C (toml_doc.cpp): paths and values as
 * wb.json's ---- */

const char *kTomlMeta = "wb.toml.document";

wb::toml::Document *toml_doc(lua_State *L) {
	return static_cast<wb::toml::Document *>(luaL_checkudata(L, 1, kTomlMeta));
}

int toml_gc(lua_State *L) {
	toml_doc(L)->~Document();
	return 0;
}

int toml_parse(lua_State *L) {
	size_t len = 0;
	const char *text = luaL_optlstring(L, 1, "", &len);
	auto d = wb::toml::Document::parse(std::string_view(text, len));
	if (!d.ok()) {
		lua_pushnil(L);
		lua_pushstring(L, d.error().c_str());
		return 2;
	}
	void *mem = lua_newuserdata(L, sizeof(wb::toml::Document));
	new (mem) wb::toml::Document(std::move(d));
	luaL_getmetatable(L, kTomlMeta);
	lua_setmetatable(L, -2);
	return 1;
}

int toml_get(lua_State *L) {
	auto v = toml_doc(L)->get(json_path(L, 2));
	if (v)
		push_json(L, *v);
	else
		lua_pushnil(L);
	return 1;
}

int toml_set(lua_State *L) {
	auto *d = toml_doc(L);
	wb::jsonc::Path path = json_path(L, 2);
	wb::jsonc::Value v = lua_to_jsonc(L, 3, 0);
	lua_pushboolean(L, d->set(path, v));
	return 1;
}

int toml_remove(lua_State *L) {
	lua_pushboolean(L, toml_doc(L)->remove(json_path(L, 2)));
	return 1;
}

int toml_keys(lua_State *L) {
	auto v = toml_doc(L)->get(json_path(L, 2));
	lua_newtable(L);
	if (v && v->type == wb::jsonc::Value::Type::Object)
		for (size_t i = 0; i < v->members.size(); ++i) {
			lua_pushlstring(L, v->members[i].first.data(), v->members[i].first.size());
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
	return 1;
}

int toml_text(lua_State *L) {
	const std::string &t = toml_doc(L)->text();
	lua_pushlstring(L, t.data(), t.size());
	return 1;
}

void push_toml_api(lua_State *L) {
	luaL_newmetatable(L, kTomlMeta);
	lua_pushcfunction(L, toml_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	lua_newtable(L);
	const luaL_Reg fns[] = {{"parse", toml_parse}, {"get", toml_get}, {"set", toml_set},
			{"remove", toml_remove}, {"keys", toml_keys}, {"text", toml_text},
			{nullptr, nullptr}};
	for (const luaL_Reg *f = fns; f->name != nullptr; ++f) {
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
}

/* ---- the page vocabulary, and strict globals (Lua) ---- */

const char *kPrelude = R"LUA(
local described, buttons, settings, values, nbuttons = nil, {}, {}, {}, 0
local update, ini, json, toml = ...

-- wb.ini: an INI file edited without disturbing it (comments, order and
-- spacing kept; new keys at the end of their section).
local Section = {}
Section.__index = Section
function Section:get(key, default)
  local v = ini.get(self.doc, self.name, key)
  if v == nil then return default end
  return v
end
function Section:set(key, value)
  if value == nil then ini.remove(self.doc, self.name, key)
  else ini.set(self.doc, self.name, key, tostring(value)) end
end
function Section:entries() return ini.entries(self.doc, self.name) end
function Section:set_all(values)
  -- Sorted: a new key's place doesn't depend on Lua's table order.
  local keys = {}
  for k in pairs(values) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do self:set(k, values[k]) end
end
local Ini = {}
Ini.__index = Ini
function Ini:section(name) return setmetatable({ doc = self.doc, name = name or "" }, Section) end
function Ini:get(key, default) return self:section(""):get(key, default) end
function Ini:set(key, value) return self:section(""):set(key, value) end
function Ini:set_all(values) return self:section(""):set_all(values) end
function Ini:entries() return self:section(""):entries() end
function Ini:sections() return ini.sections(self.doc) end
-- A [section] gone (each time it appears, its comment too); the sections
-- put in this order (those not named stay first, as they are).
function Ini:remove_section(name) return ini.remove_section(self.doc, name) end
function Ini:order(names)
  for _, name in ipairs(names) do ini.move_section_to_end(self.doc, name) end
end
function Ini:text() return ini.text(self.doc) end
wb.ini = {}
function wb.ini.parse(text) return setmetatable({ doc = ini.parse(text or "") }, Ini) end
function wb.ini.read(path) return wb.ini.parse(wb.read(path) or "") end

-- wb.json and wb.toml: files edited without disturbing them. A path is a
-- key, an index (from 1) or a table of them: cfg:get({ 1, "height" }).
local function document_class(api, what)
  local Doc = {}
  Doc.__index = Doc
  function Doc:get(path, default)
    local v = api.get(self.doc, path)
    if v == nil or v == json.null then return default end
    return v
  end
  function Doc:set(path, value)
    if value == nil then api.remove(self.doc, path) return end
    if not api.set(self.doc, path, value) then
      error("can't set " .. (type(path) == "table" and table.concat(path, ".") or
          tostring(path)) .. ": " .. what, 2)
    end
  end
  function Doc:keys(path) return api.keys(self.doc, path) end
  function Doc:set_all(values, path)
    local keys = {}
    for k in pairs(values) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do
      local p = {}
      if type(path) == "table" then for i, part in ipairs(path) do p[i] = part end
      elseif path ~= nil then p[1] = path end
      p[#p + 1] = k
      self:set(p, values[k])
    end
  end
  function Doc:text() return api.text(self.doc) end
  return function(text, name)
    local doc, err = api.parse(text or "")
    if not doc then error((name or "the file") .. ": " .. err, 3) end
    return setmetatable({ doc = doc }, Doc)
  end
end

-- JSON: comments and trailing commas allowed, as Waybar's.
local json_doc = document_class(json, "something on the way isn't an object or array")
wb.json = { null = json.null }
function wb.json.array(t) return setmetatable(t or {}, json.array_mt) end
function wb.json.parse(text, name) return json_doc(text, name or "JSON") end
function wb.json.read(path) return json_doc(wb.read(path) or "", path) end

-- TOML: as Alacritty's and wpaperd's.
local toml_doc = document_class(toml, "something on the way isn't a table, the value " ..
    "is nil inside, or it's a new element of an array of tables")
wb.toml = {}
function wb.toml.parse(text, name) return toml_doc(text, name or "TOML") end
function wb.toml.read(path) return toml_doc(wb.read(path) or "", path) end

local function distance(a, b)
  if math.abs(#a - #b) > 3 then return 99 end
  local prev = {}
  for j = 0, #b do prev[j] = j end
  for i = 1, #a do
    local cur = { [0] = i }
    for j = 1, #b do
      local cost = a:sub(i, i) == b:sub(j, j) and 0 or 1
      cur[j] = math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
    end
    prev = cur
  end
  return prev[#b]
end

local function unknown(key, level)
  local best, bd = nil, 3
  for k in pairs(settings) do
    local d = distance(tostring(key), k)
    if d < bd then best, bd = k, d end
  end
  error(string.format("'%s' is not a setting of this page%s", tostring(key),
      best and (" (" .. best .. "?)") or ""), level + 1)
end

-- A setting's constructor's registration, to undo for a sections' fields.
local prior = setmetatable({}, { __mode = "k" })

-- A sections' value as sent: names and their fields' values as text.
local function sections_value(fields, v, level)
  if type(v) ~= "table" then
    error("a sections' value is a table of { name = ..., values = { ... } }", level + 1)
  end
  local out = {}
  for i, s in ipairs(v) do
    local vals = {}
    for k, x in pairs(type(s) == "table" and s.values or {}) do
      local kind = fields[k]
      if kind == nil then error("'" .. tostring(k) .. "' isn't one of its fields", level + 1) end
      local t
      if x == nil then t = ""
      elseif kind == "switch" and type(x) == "boolean" then t = x and "on" or ""
      else t = tostring(x) end
      if t ~= "" then vals[k] = t end
    end
    out[i] = { name = tostring(type(s) == "table" and s.name or s), values = vals }
  end
  return json.wire_encode(out)
end

local function as_value(kind, v, key)
  if v == nil then v = kind == "sections" and {} or nil end
  if kind == "sections" then
    local text = sections_value(settings[key].fields, v, 3)  -- not a tail call: levels
    return text
  end
  if v == nil then return "" end
  if kind == "switch" and type(v) == "boolean" then return v and "on" or "" end
  if kind == "list" then
    if type(v) ~= "table" then error("a list's value is a table of names", 3) end
    local names = {}
    for i, name in ipairs(v) do names[i] = tostring(name) end
    return table.concat(names, "\n")
  end
  return tostring(v)
end

-- What a page gets: a list's names as a table, the rest as text.
local function from_wire(kind, text)
  if kind == "sections" then
    local out = json.wire_decode(text)
    for _, s in ipairs(out) do s.values = s.values or {} end
    return out
  end
  if kind ~= "list" then return text end
  local names = {}
  for name in (text or ""):gmatch("[^\n]+") do names[#names + 1] = name end
  return names
end

local function setting(kind)
  return function(t)
    if type(t) ~= "table" then
      error(kind .. ' { "key", ... } takes a table', 2)
    end
    local key = t[1] or t.key
    if type(key) ~= "string" or key == "" then
      error(kind .. ' needs its key first: ' .. kind .. ' { "key", label = ... }', 2)
    end
    local c = { kind = kind, key = key }
    for k, v in pairs(t) do
      if type(k) == "string" and k ~= "key" and k ~= "on_change" then c[k] = v end
    end
    prior[c] = { value = values[key], setting = settings[key] }
    c.value = as_value(kind, t.value)
    values[key] = c.value
    settings[key] = { kind = kind, on_change = t.on_change }
    return c
  end
end

for _, kind in ipairs { "text", "number", "switch", "choice", "color", "file", "folder", "image",
                        "list", "font" } do
  _G[kind] = setting(kind)
end

-- Repeated sections: each named, with the fields given (made with text,
-- number, color...; theirs, not the page's settings).
function sections(t)
  if type(t) ~= "table" then error('sections { "key", fields = { ... } } takes a table', 2) end
  local key = t[1] or t.key
  if type(key) ~= "string" or key == "" then
    error('sections needs its key first: sections { "key", fields = { ... } }', 2)
  end
  local kinds, children = {}, {}
  for i, f in ipairs(t.fields or {}) do
    if type(f) ~= "table" or not f.key or not prior[f] then
      error("a sections' fields are settings: text { ... }, number { ... }...", 2)
    end
    -- Not the page's: whatever the key was before, back.
    local p = prior[f]
    values[f.key] = p.value
    settings[f.key] = p.setting
    f.value = nil
    kinds[f.key] = f.kind
    children[i] = f
  end
  local c = { kind = "sections", key = key, children = children }
  for k, v in pairs(t) do
    if type(k) == "string" and k ~= "key" and k ~= "on_change" and k ~= "fields" and
        k ~= "value" then
      c[k] = v
    end
  end
  settings[key] = { kind = "sections", fields = kinds, on_change = t.on_change }
  c.value = sections_value(kinds, t.value or {}, 2)
  values[key] = c.value
  return c
end

function group(title)
  local function make(label, list, from)
    local c = { kind = "group", label = label, children = {} }
    for i = from, #list do c.children[#c.children + 1] = list[i] end
    return c
  end
  if type(title) == "table" then return make(title[1] or title.label, title, 2) end
  return function(children) return make(title, children, 1) end
end

function label(t)
  if type(t) == "string" then return { kind = "label", label = t } end
  return { kind = "label", label = t[1] or t.label, key = t.key }
end

function button(t)
  if type(t) ~= "table" then error('button { "Label", on_click = function() ... end }', 2) end
  local key = t.key
  if not key then nbuttons = nbuttons + 1; key = "button" .. nbuttons end
  buttons[key] = t.on_click
  return { kind = "button", key = key, label = t[1] or t.label, description = t.description }
end

function page(t)
  if described then error("page { ... } is given twice", 2) end
  local cs = {}
  for i, c in ipairs(t) do cs[i] = c end
  described = { controls = cs }
end

wb.get = function(key)
  if values[key] == nil then unknown(key, 2) end
  return from_wire(settings[key].kind, values[key])
end
wb.set = function(key, fields)
  if values[key] == nil and not buttons[key] then unknown(key, 2) end
  if type(fields) ~= "table" then
    error("wb.set(key, { value = ..., visible = ..., enabled = ..., problem = ... })", 2)
  end
  if fields.value ~= nil and values[key] ~= nil then
    fields.value = as_value(settings[key].kind, fields.value, key)
    values[key] = fields.value
  end
  update(key, fields)
end
wb.problem = function(key, text) wb.set(key, { problem = text or "" }) end

local api = {}
function api.described() return described end
function api.change(key, value)
  if values[key] == nil then unknown(key, 1) end
  values[key] = value
  local s = settings[key]
  local v = from_wire(s.kind, value)
  if s.on_change then s.on_change(v) end
  local f = rawget(_G, "on_change")
  if f then f(key, v) end
end
function api.click(key)
  local f = buttons[key]
  if f == nil then error("no button '" .. tostring(key) .. "'") end
  if f then f() end
end
function api.save(given)
  for k, v in pairs(given) do if values[k] ~= nil then values[k] = v end end
  local f = rawget(_G, "on_save")
  if f then
    local copy = {}
    for k, v in pairs(values) do copy[k] = from_wire(settings[k].kind, v) end
    f(copy)
  end
end

-- Strict globals: a typo is an error, said where it is.
local allowed = { on_change = true, on_save = true }
setmetatable(_G, {
  __newindex = function(t, k, v)
    if allowed[k] then rawset(t, k, v); return end
    error("assigning the global '" .. tostring(k) .. "': declare it with 'local' first", 2)
  end,
  __index = function(_, k)
    error("'" .. tostring(k) .. "' isn't defined (a typo, or a missing 'local'?)", 2)
  end,
})
return api
)LUA";

void open_lib(lua_State *L, const char *name, lua_CFunction f) {
#if LUA_VERSION_NUM >= 502
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
#else
	lua_pushcfunction(L, f);
	lua_pushstring(L, name);
	lua_call(L, 1, 0);
#endif
}

void open_sandbox(lua_State *L) {
	/* Only these: not luaL_openlibs, whose "jit" library (LuaJIT) starts
	 * the compiler, and compiled code never runs the hook enforcing the
	 * time budget: an endless loop would never stop. */
#if LUA_VERSION_NUM >= 502
	open_lib(L, "_G", luaopen_base);
#else
	open_lib(L, "", luaopen_base);
#endif
	open_lib(L, LUA_TABLIBNAME, luaopen_table);
	open_lib(L, LUA_STRLIBNAME, luaopen_string);
	open_lib(L, LUA_MATHLIBNAME, luaopen_math);
#if LUA_VERSION_NUM >= 503
	open_lib(L, LUA_UTF8LIBNAME, luaopen_utf8);
#endif
#ifdef LUA_BITLIBNAME
	open_lib(L, LUA_BITLIBNAME, luaopen_bit);  /* LuaJIT */
#endif
	/* Out: everything that reaches outside the page. */
	for (const char *g : {"io", "os", "package", "debug", "require", "dofile", "loadfile",
			"load", "loadstring", "module", "collectgarbage", "jit", "ffi", "newproxy",
			"getfenv", "setfenv", "rawequal", "print"}) {
		lua_pushnil(L);
		lua_setglobal(L, g);
	}
	/* string.dump would hand out bytecode; string's metatable stays. */
	lua_getglobal(L, "string");
	lua_pushnil(L);
	lua_setfield(L, -2, "dump");
	lua_pop(L, 1);
	lua_newtable(L);
	const luaL_Reg fns[] = {{"read", wb_read}, {"write", wb_write}, {"run", wb_run}, {"reload", wb_reload},
		{"option", wb_option},
			{"log", wb_log}, {nullptr, nullptr}};
	for (const luaL_Reg *f = fns; f->name != nullptr; ++f) {
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setglobal(L, "wb");
	lua_pushcfunction(L, wb_log);
	lua_setglobal(L, "print");  /* to wbconf's log, as wb.log */
}

bool open_page(const json::Value &msg) {
	std::string script = msg.str("script");
	R.home = msg.str("home");
	auto strings = [&msg](const char *k) {
		std::vector<std::string> out;
		if (const json::Value *a = msg.get(k); a != nullptr && a->array() != nullptr)
			for (const json::Value &e : *a->array())
				if (e.string() != nullptr)
					out.push_back(*e.string());
		return out;
	};
	R.files = strings("files");
	R.runs = strings("runs");
	R.options.clear();
	if (const json::Value *o = msg.get("options"); o != nullptr && o->object() != nullptr)
		for (const auto &[k, v] : *o->object())
			if (v.string() != nullptr)
				R.options[k] = *v.string();
	size_t slash = script.rfind('/');
	R.script_name = slash == std::string::npos ? script : script.substr(slash + 1);

	R.L = luaL_newstate();
	if (R.L == nullptr) {
		emit_error("can't start Lua");
		return false;
	}
	lua_State *L = R.L;
	open_sandbox(L);
	lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
	R.deadline = now_ms() + kOpenMs;
	if (luaL_loadbuffer(L, kPrelude, strlen(kPrelude), "=prelude") != 0) {
		emit_error(lua_tostring(L, -1));
		return false;
	}
	lua_pushcfunction(L, wb_update);
	push_ini_api(L);
	push_json_api(L);
	push_toml_api(L);
	if (lua_pcall(L, 4, 1, 0) != 0) {
		emit_error(lua_tostring(L, -1));
		return false;
	}
	R.api_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	std::ifstream f(script, std::ios::binary);
	if (!f) {
		emit_error("the page's script can't be read: " + script);
		return false;
	}
	std::stringstream text;
	text << f.rdbuf();
	std::string code = text.str();
	/* Source only: compiled Lua (bytecode, which every Lua's and LuaJIT's
	 * starts with ESC) is refused before anything loads it. */
	if (!code.empty() && code[0] == '\x1b') {
		emit_error(R.script_name + ": compiled Lua isn't a page; it must be Lua source");
		return false;
	}
#if LUA_VERSION_NUM >= 502
	int loaded = luaL_loadbufferx(L, code.data(), code.size(), ("@" + R.script_name).c_str(), "t");
#else
	int loaded = luaL_loadbuffer(L, code.data(), code.size(), ("@" + R.script_name).c_str());
#endif
	if (loaded != 0) {
		emit_error(lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	if (!call(0, kOpenMs))
		return false;
	lua_rawgeti(L, LUA_REGISTRYINDEX, R.api_ref);
	lua_getfield(L, -1, "described");
	lua_remove(L, -2);
	if (lua_pcall(L, 0, 1, 0) != 0 || lua_isnil(L, -1)) {
		lua_pop(L, 1);
		emit_error("the script describes no page: page { ... } is missing");
		return false;
	}
	json::Value described = to_json(L, -1, 0);
	lua_pop(L, 1);
	std::string why;
	auto p = wb::page::page_from_json(described, &why);
	if (!p) {
		emit_error("the page can't be drawn: " + why);
		return false;
	}
	/* The page first, then what its script asked for while loading. */
	R.out.insert(R.out.begin(), json::Object{{"page", wb::page::page_to_json(*p)}});
	return true;
}

/* api[name](args...) */
void call_api(const char *name, const std::vector<json::Value> &args) {
	lua_State *L = R.L;
	lua_rawgeti(L, LUA_REGISTRYINDEX, R.api_ref);
	lua_getfield(L, -1, name);
	lua_remove(L, -2);
	for (const json::Value &a : args)
		push_json(L, a);
	call(static_cast<int>(args.size()), kCallMs);
}

}  // namespace

int main() {
	/* A runaway page can't take the machine's memory with it. (Not under
	 * AddressSanitizer, whose shadow memory alone is terabytes of address
	 * space.) */
#if defined(__SANITIZE_ADDRESS__)
#define WB_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WB_ASAN 1
#endif
#endif
#ifndef WB_ASAN
	struct rlimit lim = {kMemory, kMemory};
	setrlimit(RLIMIT_AS, &lim);
#else
	(void)kMemory;
#endif
	std::ios::sync_with_stdio(false);
	std::string line;
	bool opened = false;
	while (std::getline(std::cin, line)) {
		auto msg = json::parse(line);
		if (!msg || msg->object() == nullptr) {
			emit_error("not a message: " + line.substr(0, 80));
			flush();
			continue;
		}
		if (const json::Value *o = msg->get("open")) {
			if (opened) {
				emit_error("the page is open already");
			} else {
				opened = true;
				if (!open_page(*o)) {
					flush();
					return 1;  /* nothing to run */
				}
			}
		} else if (!opened) {
			emit_error("open the page first");
		} else if (const json::Value *c = msg->get("change")) {
			call_api("change", {json::Value(c->str("key")), json::Value(c->str("value"))});
		} else if (const json::Value *k = msg->get("click"); k != nullptr && k->string()) {
			call_api("click", {*k});
		} else if (const json::Value *s = msg->get("save"); s != nullptr && s->object()) {
			call_api("save", {*s});
		} else {
			emit_error("unknown message: " + line.substr(0, 80));
		}
		flush();
	}
	if (R.L != nullptr)
		lua_close(R.L);
	return 0;
}
