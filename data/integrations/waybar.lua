-- Waybar's settings page (waybox-integration(5), LUA PAGES): the bar's
-- place, size and modules, in its own config file (JSON with comments),
-- changed in place. Only what's changed, or already in the file, is
-- written; the rest are Waybar's defaults (waybar(5)). Saved, Waybar
-- reloads (Reload=signal USR2).
local mine = { "~/.config/waybar/config.jsonc", "~/.config/waybar/config" }
local default = "/etc/xdg/waybar/config.jsonc"

-- The file Waybar reads: the user's, else the default (saving then makes
-- the user's own copy of it).
local path, raw = mine[1], nil
for _, p in ipairs(mine) do
  raw = wb.read(p)
  if raw then path = p break end
end
local own = raw ~= nil
if not own then
  local ok, t = pcall(wb.read, default)  -- not installed with it: none
  raw = ok and t or ""
end
local cfg = wb.json.parse(raw, own and path or default)

-- One bar (an object), or several (an array): this page edits the first.
local bars = cfg:get({}, {})
local several = #bars > 0
local at = several and { 1 } or {}
-- A path in that bar: key("clock", "format").
local function key(...)
  local p = {}
  for i, part in ipairs(at) do p[i] = part end
  for _, part in ipairs({ ... }) do p[#p + 1] = part end
  return p
end

local defaults = { position = "top", layer = "bottom", height = 0, spacing = 0,
  margin = "", exclusive = true }
local function now(k)
  local v = cfg:get(key(k))
  if v == nil then return defaults[k] end
  return v
end

-- A list of names as text ("eDP-1, HDMI-A-1"), and back.
local function joined(list)
  if type(list) == "string" then return list end
  if type(list) ~= "table" then return "" end
  return table.concat(list, ", ")
end
local function split(s)
  local out = {}
  for name in (s or ""):gmatch("[^,%s]+") do out[#out + 1] = name end
  return out
end
local function names(v)
  if type(v) ~= "table" then return {} end
  local out = {}
  for i, name in ipairs(v) do out[i] = tostring(name) end
  return out
end
local function has_clock(...)
  for _, list in ipairs({ ... }) do
    for _, name in ipairs(list) do
      if name == "clock" or name:match("^clock#") then return true end
    end
  end
  return false
end

local modules = {
  left = names(cfg:get(key("modules-left"))),
  center = names(cfg:get(key("modules-center"))),
  right = names(cfg:get(key("modules-right"))),
}

-- To add: Waybar's usual modules, and those this config sets up
-- (custom/media, battery#bat2).
local known = {
  { "ext/workspaces", "Desktops" }, { "wlr/taskbar", "Windows" }, { "clock", "Clock" },
  { "tray", "System tray" }, { "pulseaudio", "Volume" }, { "wireplumber", "Volume" },
  { "network", "Network" }, { "bluetooth", "Bluetooth" }, { "battery", "Battery" },
  { "backlight", "Brightness" }, { "power-profiles-daemon", "Power profile" },
  { "idle_inhibitor", "Keep awake" }, { "cpu", "Processor" }, { "memory", "Memory" },
  { "temperature", "Temperature" }, { "disk", "Disk" }, { "mpris", "Media player" },
  { "privacy", "Screen sharing and microphone" }, { "keyboard-state", "Caps and Num Lock" },
  { "user", "User" },
}
local suggestions, seen = {}, {}
for _, k in ipairs(known) do suggestions[#suggestions + 1] = k; seen[k[1]] = true end
local bar = cfg:get(at, {})
if type(bar) == "table" then
  local configured = {}
  for name, v in pairs(bar) do
    if type(v) == "table" and not seen[name] and (name:find("/") or name:find("#")) then
      configured[#configured + 1] = name
    end
  end
  table.sort(configured)
  for _, name in ipairs(configured) do suggestions[#suggestions + 1] = name end
end

local controls = {}
if not own then
  controls[#controls + 1] = label("Waybar uses its default settings now. Saving here makes your " ..
      "own copy of them, in " .. mine[1] .. ".")
end
if several and #bars > 1 then
  controls[#controls + 1] = label(string.format("Your config has %d bars: this page sets the first.",
      #bars))
end

local groups = {
  group "Where" {
    choice { "position", label = "Edge", value = now("position"),
             choices = { { "top", "Top" }, { "bottom", "Bottom" }, { "left", "Left" },
                         { "right", "Right" } } },
    choice { "layer", label = "Layer", value = now("layer"),
             choices = { { "bottom", "Below windows" }, { "top", "Above windows" },
                         { "overlay", "Above everything" } },
             description = "Above everything includes fullscreen windows." },
    text { "output", label = "Monitors", value = joined(cfg:get(key("output"))), hint = "All",
           description = "Monitor names, as Monitors shows them, separated by commas; " ..
               "empty for every monitor." },
    switch { "exclusive", label = "Keep windows clear of it", value = now("exclusive") },
  },
  group "Size" {
    number { "height", label = "Height", unit = "px", min = 0, max = 1000,
             value = now("height"), description = "0: as tall as what's in it." },
    number { "spacing", label = "Between modules", unit = "px", min = 0, max = 200,
             value = now("spacing") },
    text { "margin", label = "Margin", value = now("margin"), hint = "0",
           description = "Pixels around the bar: one value, or up to four (top, right, " ..
               "bottom, left), separated by spaces." },
  },
  group "Modules" {
    list { "modules-left", label = "Left", value = modules.left, choices = suggestions,
           hint = "A module, then Enter" },
    list { "modules-center", label = "Middle", value = modules.center, choices = suggestions,
           hint = "A module, then Enter" },
    list { "modules-right", label = "Right", value = modules.right, choices = suggestions,
           hint = "A module, then Enter",
           description = "Each module's own settings are in the config file, under its name." },
  },
  group "Clock" {
    text { "clock-format", label = "Format", value = cfg:get(key("clock", "format"), ""),
           hint = "{:%H:%M}",
           description = "As strftime, in braces: {:%a %d %b %H:%M} for Thu 26 Nov 14:05." },
  },
}
for _, g in ipairs(groups) do controls[#controls + 1] = g end
page(controls)

local function show_clock(v)
  wb.set("clock-format", { visible = has_clock(v["modules-left"], v["modules-center"],
      v["modules-right"]) })
end
show_clock({ ["modules-left"] = modules.left, ["modules-center"] = modules.center,
             ["modules-right"] = modules.right })

function on_change(k, value)
  if k == "margin" then
    local ok = value == "" or value:match("^%s*%d+%s*$") or value:match("^%s*%d+%s+%d+%s*$") or
        value:match("^%s*%d+%s+%d+%s+%d+%s*$") or value:match("^%s*%d+%s+%d+%s+%d+%s+%d+%s*$")
    wb.problem("margin", ok and "" or "Up to four numbers, separated by spaces.")
  elseif k:match("^modules%-") then
    show_clock({ ["modules-left"] = wb.get("modules-left"),
                 ["modules-center"] = wb.get("modules-center"),
                 ["modules-right"] = wb.get("modules-right") })
  end
end

local function same_list(a, b)
  if type(a) ~= "table" or #a ~= #b then return false end
  for i = 1, #b do if a[i] ~= b[i] then return false end end
  return true
end

function on_save(v)
  -- A setting written only when it's changed from Waybar's default, or
  -- the file has it already.
  local function put(k, value, default_value)
    if value == "" then
      cfg:set(key(k), nil)  -- cleared: Waybar's own, the key gone, not ""
    elseif cfg:get(key(k)) ~= nil or value ~= default_value then
      cfg:set(key(k), value)
    end
  end
  put("position", v.position, defaults.position)
  put("layer", v.layer, defaults.layer)
  put("exclusive", v.exclusive == "on", defaults.exclusive)
  put("height", tonumber(v.height) or 0, defaults.height)
  put("spacing", tonumber(v.spacing) or 0, defaults.spacing)
  put("margin", v.margin, defaults.margin)
  local outputs = split(v.output)
  if #outputs == 0 then
    cfg:set(key("output"), nil)
  elseif not same_list(cfg:get(key("output")), outputs) and
      cfg:get(key("output")) ~= v.output then
    cfg:set(key("output"), #outputs == 1 and outputs[1] or outputs)
  end
  for _, side in ipairs({ "modules-left", "modules-center", "modules-right" }) do
    local list = v[side]
    if not same_list(cfg:get(key(side)), list) then
      cfg:set(key(side), #list > 0 and list or nil)
    end
  end
  if v["clock-format"] ~= cfg:get(key("clock", "format"), "") then
    cfg:set(key("clock", "format"), v["clock-format"] ~= "" and v["clock-format"] or nil)
  end
  -- The user's file; without one, their own copy of the default.
  wb.write(own and path or mine[1], cfg:text())
  wb.reload()
end
