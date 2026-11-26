---@meta
-- Type hints for integrations' Lua pages (waybox-integration(5), LUA PAGES),
-- for the Lua Language Server: point "workspace.library" at this file's
-- folder (installed to share/waybox/lua) to have editors complete and check
-- a page. Nothing here runs: wbconf-lua provides all of it.

---@alias WbValue string|number|boolean

---@class WbControl
---@field [1] string                  the setting's key (or a button's label)
---@field key? string
---@field label? string
---@field description? string
---@field value? any
---@field hint? string
---@field unit? string
---@field min? number                 a number's
---@field max? number
---@field choices? (string|string[])[] a choice's values, or { value, label }
---@field wide? boolean               a text taking the row's width
---@field format? "pango"|"fontconfig" a font's
---@field monospace? boolean          a font's list starting with monospace ones
---@field fields? WbControl[]         sections': their fields
---@field on_change? fun(value: any)
---@field on_click? fun()             a button's

--- The page: its controls in order. Given once.
---@param controls WbControl[]
function page(controls) end

--- A titled group of controls: `group "Title" { ... }`.
---@param title string|table
---@return fun(controls: WbControl[]): WbControl
function group(title) end

--- A line of text.
---@param text string|{ [1]: string, key?: string }
---@return WbControl
function label(text) end

--- A button: `button { "Label", on_click = function() ... end }`.
---@param t { [1]: string, key?: string, description?: string, on_click: fun() }
---@return WbControl
function button(t) end

--- Settings, their key first: `number { "width", label = "Width", value = 300 }`.
---@param t WbControl
---@return WbControl
function text(t) end
---@param t WbControl
---@return WbControl
function number(t) end
---@param t WbControl
---@return WbControl
function switch(t) end
---@param t WbControl
---@return WbControl
function choice(t) end
---@param t WbControl
---@return WbControl
function color(t) end
---@param t WbControl
---@return WbControl
function file(t) end
---@param t WbControl
---@return WbControl
function folder(t) end
---@param t WbControl
---@return WbControl
function image(t) end
--- Names in order (moved, removed, added); its value a table of names.
---@param t WbControl
---@return WbControl
function list(t) end
--- A font: family, size, style; written as `format` says (pango by default).
---@param t WbControl
---@return WbControl
function font(t) end
--- Repeated sections, each named, with `fields`; its value a table of
--- `{ name = ..., values = { field = text } }`.
---@param t WbControl
---@return WbControl
function sections(t) end

--- A setting changed: its key and new value (a list's and sections' as tables).
---@type fun(key: string, value: any)?
on_change = nil

--- Save & Apply: every setting's value, by key. Write the program's files here.
---@type fun(values: table<string, any>)?
on_save = nil

---@class WbSectionDoc
local Section = {}
---@param key string
---@param default? string
---@return string?
function Section:get(key, default) end
--- nil removes the key.
---@param key string
---@param value? WbValue
function Section:set(key, value) end
---@param values table<string, WbValue?>
function Section:set_all(values) end
---@return [string, string][]
function Section:entries() end

---@class WbIniDoc : WbSectionDoc
local Ini = {}
--- The keys before any [section] (the file's own).
---@param key string
---@param default? string
---@return string?
function Ini:get(key, default) end
---@param key string
---@param value? WbValue
function Ini:set(key, value) end
---@param values table<string, WbValue?>
function Ini:set_all(values) end
---@return [string, string][]
function Ini:entries() end
--- A [section] ("" or nil: the keys before any).
---@param name? string
---@return WbSectionDoc
function Ini:section(name) end
---@return string[]
function Ini:sections() end
---@param name string
---@return boolean
function Ini:remove_section(name) end
--- The sections put in this order.
---@param names string[]
function Ini:order(names) end
--- The file's text, to write.
---@return string
function Ini:text() end

--- A path: a key, an index (from 1), or a table of them: { 1, "clock", "format" }.
---@alias WbPath string|integer|(string|integer)[]

---@class WbDoc
local Doc = {}
---@param path WbPath
---@param default? any
---@return any
function Doc:get(path, default) end
--- nil removes it; tables on the way are made.
---@param path WbPath
---@param value any
function Doc:set(path, value) end
---@param values table<string, any>
---@param path? WbPath
function Doc:set_all(values, path) end
---@param path? WbPath
---@return string[]
function Doc:keys(path) end
---@return string
function Doc:text() end

---@class wb
wb = {}

--- A file's text, if it's in the integration's Files=; nil if it can't be read.
---@param path string
---@return string?
function wb.read(path) end

--- A file to write on Save & Apply (only one in Files=; the old one kept as .bak).
---@param path string
---@param text string
function wb.write(path, text) end

--- A program to run (one in Runs=), with its arguments.
---@param argv string[]
function wb.run(argv) end

--- One of the integration's options ([Option ID]) as the program's command
--- sets it now (waybar's -c FILE), or nil. A file or folder named this way
--- may be read and written, as if in Files=, once the user has set it (not
--- the one Exec= gives: that goes in Files=).
---@param id string
---@return string?
function wb.option(id) end

--- The program takes its settings again, as the integration's Reload= says.
function wb.reload() end

--- To wbconf's log.
---@param ... any
function wb.log(...) end

--- A setting's value (a list's and sections' as tables).
---@param key string
---@return any
function wb.get(key) end

--- Change a control: { value = ..., visible = ..., enabled = ..., problem = ..., label = ... }.
---@param key string
---@param fields { value?: any, visible?: boolean, enabled?: boolean, problem?: string, label?: string }
function wb.set(key, fields) end

--- A problem shown under a setting ("" clears it).
---@param key string
---@param text string
function wb.problem(key, text) end

wb.ini = {}
--- An INI file, edited in place (comments, order and spacing kept).
---@param path string
---@return WbIniDoc
function wb.ini.read(path) end
---@param text string
---@return WbIniDoc
function wb.ini.parse(text) end

wb.json = {}
--- JSON with comments and trailing commas (Waybar's), edited in place.
---@param path string
---@return WbDoc
function wb.json.read(path) end
---@param text string
---@param name? string
---@return WbDoc
function wb.json.parse(text, name) end
--- A table written as an array, even empty.
---@param t? table
---@return table
function wb.json.array(t) end
--- JSON's null, as read.
wb.json.null = nil

wb.toml = {}
--- TOML (Alacritty's, wpaperd's), edited in place.
---@param path string
---@return WbDoc
function wb.toml.read(path) end
---@param text string
---@param name? string
---@return WbDoc
function wb.toml.parse(text, name) end
