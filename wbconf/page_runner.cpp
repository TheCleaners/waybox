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
			{"text", ini_text}, {nullptr, nullptr}};
	for (const luaL_Reg *f = fns; f->name != nullptr; ++f) {
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
}

/* ---- the page vocabulary, and strict globals (Lua) ---- */

const char *kPrelude = R"LUA(
local described, buttons, settings, values, nbuttons = nil, {}, {}, {}, 0
local update, ini = ...

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
function Ini:text() return ini.text(self.doc) end
wb.ini = {}
function wb.ini.parse(text) return setmetatable({ doc = ini.parse(text or "") }, Ini) end
function wb.ini.read(path) return wb.ini.parse(wb.read(path) or "") end

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

local function as_value(kind, v)
  if v == nil then return "" end
  if kind == "switch" and type(v) == "boolean" then return v and "on" or "" end
  return tostring(v)
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
    c.value = as_value(kind, t.value)
    values[key] = c.value
    settings[key] = { kind = kind, on_change = t.on_change }
    return c
  end
end

for _, kind in ipairs { "text", "number", "switch", "choice", "color", "file", "folder", "image" } do
  _G[kind] = setting(kind)
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
  return values[key]
end
wb.set = function(key, fields)
  if values[key] == nil and not buttons[key] then unknown(key, 2) end
  if type(fields) ~= "table" then
    error("wb.set(key, { value = ..., visible = ..., enabled = ..., problem = ... })", 2)
  end
  if fields.value ~= nil and values[key] ~= nil then
    fields.value = as_value(settings[key].kind, fields.value)
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
  if s.on_change then s.on_change(value) end
  local f = rawget(_G, "on_change")
  if f then f(key, value) end
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
    for k, v in pairs(values) do copy[k] = v end
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
	const luaL_Reg fns[] = {{"read", wb_read}, {"write", wb_write}, {"run", wb_run},
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
	if (lua_pcall(L, 2, 1, 0) != 0) {
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
	if (luaL_loadbuffer(L, code.data(), code.size(), ("@" + R.script_name).c_str()) != 0) {
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
