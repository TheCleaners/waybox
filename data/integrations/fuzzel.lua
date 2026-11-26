-- Fuzzel's settings page (waybox-integration(5), LUA PAGES): its own
-- config file (fuzzel.ini(5)), changed in place. Only what's changed from
-- fuzzel's default, or already in the file, is written; a setting left
-- empty is fuzzel's own (its line taken out). Fuzzel reads the file each
-- time it opens (Reload=none).
local path = "~/.config/fuzzel/fuzzel.ini"
local cfg = wb.ini.read(path)

-- Each setting: its [section], key and fuzzel's default (as written).
local where = {
  font = "main", anchor = "main", layer = "main", lines = "main", width = "main",
  ["line-height"] = "main", ["letter-spacing"] = "main", ["icons-enabled"] = "main",
  prompt = "main", ["match-mode"] = "main", terminal = "main",
  ["border-width"] = "border", ["border-radius"] = "border",
}
local defaults = {
  font = "monospace", anchor = "center", layer = "overlay", lines = "15", width = "30",
  ["line-height"] = "", ["letter-spacing"] = "0", ["icons-enabled"] = "yes",
  prompt = "> ", ["match-mode"] = "fzf", terminal = "",
  ["border-width"] = "1", ["border-radius"] = "10",
}
-- The colors ([colors], RRGGBBAA without '#'), with fuzzel's defaults.
local colors = {
  { "background", "Background", "fdf6e3ff" }, { "text", "Text", "657b83ff" },
  { "match", "Matching letters", "cb4b16ff" }, { "selection", "Selected", "eee8d5ff" },
  { "selection-text", "Selected text", "586e75ff" },
  { "selection-match", "Selected matching letters", "cb4b16ff" },
  { "border", "Border", "002b36ff" },
}
for _, c in ipairs(colors) do where["color-" .. c[1]] = "colors"; defaults["color-" .. c[1]] = c[3] end

-- Its main settings: before any section (as fuzzel writes them), or in
-- [main] if the file has one.
local has_main = false
for _, name in ipairs(cfg:sections()) do if name == "main" then has_main = true end end
local function section_of(k)
  if where[k] ~= "main" then return cfg:section(where[k]) end
  return cfg:section(has_main and "main" or "")
end
local function stored(k, key)
  if where[k] ~= "main" then return cfg:section(where[k]):get(key) end
  local v = cfg:section("main"):get(key)
  if v == nil then v = cfg:section(""):get(key) end
  return v
end

-- The key in its section: "border-width" is [border] width.
local function file_key(k)
  if k:match("^border%-") then return (k:gsub("^border%-", "")) end
  if k:match("^color%-") then return (k:gsub("^color%-", "")) end
  return k
end

-- A value as shown: a color with '#'; a quoted one ("> ") without quotes.
local function shown(k, v)
  if v == nil then return nil end
  if k:match("^color%-") then return "#" .. v end
  local q = v:match('^"(.*)"$')
  return q or v
end

-- As written: a color without '#' (and opaque if no alpha is given); text
-- with spaces at either end quoted, as fuzzel takes it.
local function written(k, v)
  if k:match("^color%-") then
    v = v:gsub("^#", "")
    if #v == 6 then v = v .. "ff" end
    return v:lower()
  end
  if v:match("^%s") or v:match("%s$") then return '"' .. v .. '"' end
  return v
end

local function now(k)
  local v = shown(k, stored(k, file_key(k)))
  if v == nil then
    v = defaults[k]
    if k:match("^color%-") then v = "#" .. v end
  end
  if k == "icons-enabled" then return v ~= "no" end
  return v
end

local colors_group = { "Colors" }
for _, c in ipairs(colors) do
  colors_group[#colors_group + 1] = color { "color-" .. c[1], label = c[2],
                                            value = now("color-" .. c[1]) }
end

page {
  group "Where" {
    choice { "anchor", label = "Opens", value = now("anchor"),
             choices = { { "center", "In the middle" }, { "top", "At the top" },
                         { "top-left", "Top left" }, { "top-right", "Top right" },
                         { "bottom", "At the bottom" }, { "bottom-left", "Bottom left" },
                         { "bottom-right", "Bottom right" }, { "left", "On the left" },
                         { "right", "On the right" } } },
    choice { "layer", label = "Layer", value = now("layer"),
             choices = { { "overlay", "Above everything" }, { "top", "Above windows" } },
             description = "Above everything includes fullscreen windows." },
    number { "lines", label = "Programs shown", min = 1, max = 100, value = now("lines") },
    number { "width", label = "Width", unit = "characters", min = 10, max = 300,
             value = now("width") },
  },
  group "Look" {
    font { "font", label = "Font", format = "fontconfig", hint = "monospace",
           value = now("font") },
    text { "line-height", label = "Line height", value = now("line-height"),
           hint = "The font's own",
           description = "In pixels, or with pt for points (20, 14pt)." },
    text { "letter-spacing", label = "Letter spacing", value = now("letter-spacing"),
           hint = "0", description = "In pixels, or with pt; below 0 is tighter." },
    switch { "icons-enabled", label = "Icons", value = now("icons-enabled") },
    text { "prompt", label = "Prompt", value = now("prompt"), hint = "> " },
  },
  group(colors_group),
  group "Border" {
    number { "border-width", label = "Width", unit = "px", min = 0, max = 50,
             value = now("border-width") },
    number { "border-radius", label = "Corner radius", unit = "px", min = 0, max = 100,
             value = now("border-radius") },
  },
  group "Finding" {
    choice { "match-mode", label = "Matching", value = now("match-mode"),
             choices = { { "fzf", "Letters in order, best first" },
                         { "fuzzy", "Letters in order, close together" },
                         { "exact", "The exact text" } } },
    text { "terminal", label = "Terminal", value = now("terminal"), hint = "$TERMINAL -e",
           description = "For programs that run in a terminal: the command, with its " ..
               "option to run one (foot -e, kitty --)." },
  },
}

local function unit_ok(v)
  return v == "" or v:match("^%-?%d+%.?%d*$") or v:match("^%-?%d+%.?%d*pt$") or
      v:match("^%-?%d+%.?%d*px$")
end

function on_change(k, v)
  if k == "line-height" or k == "letter-spacing" then
    wb.problem(k, unit_ok(v) and "" or "A number, with pt or px after it if you like.")
  end
end

function on_save(values)
  -- In order: a new key's place doesn't depend on Lua's table order.
  local keys = {}
  for k in pairs(values) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    local v = values[k]
    -- One that isn't valid (said under it): the file's left as it was.
    if (k == "line-height" or k == "letter-spacing") and not unit_ok(v) then
      v = nil
    end
    if v == nil then goto next end
    local key = file_key(k)
    if k == "icons-enabled" then v = v == "on" and "yes" or "no" end
    local there = stored(k, key) ~= nil
    if v == "" then
      -- fuzzel's own: the line out, wherever it is
      if where[k] == "main" then
        cfg:section("main"):set(key, nil)
        cfg:section(""):set(key, nil)
      else
        cfg:section(where[k]):set(key, nil)
      end
    else
      local default = defaults[k]
      if k:match("^color%-") then default = "#" .. default end
      local out = written(k, v)
      if there or default == "" or out ~= written(k, default) then
        -- Where it is, or where its section's go.
        local sec = section_of(k)
        if where[k] == "main" and cfg:section("main"):get(key) == nil and
            cfg:section(""):get(key) ~= nil then
          sec = cfg:section("")
        end
        sec:set(key, out)
      end
    end
    ::next::
  end
  wb.write(path, cfg:text())
  wb.reload()
end
