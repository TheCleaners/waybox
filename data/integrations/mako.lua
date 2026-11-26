-- mako's settings page (waybox-integration(5), LUA PAGES): its own config
-- file, read and written in place. A setting is written only when it's
-- changed, or the file has it already, so the file stays as short as the
-- user keeps it; the rest are mako's defaults (mako(5)).
local path = "~/.config/mako/config"
local cfg = wb.ini.read(path)

local defaults = {
  anchor = "top-right", layer = "top",
  width = "300", height = "100", margin = "10", padding = "5",
  ["border-size"] = "2", ["border-radius"] = "0",
  font = "monospace 10", ["background-color"] = "#285577FF",
  ["text-color"] = "#FFFFFFFF", ["border-color"] = "#4C7899FF",
  icons = "1", ["max-icon-size"] = "64",
  ["default-timeout"] = "0", ["ignore-timeout"] = "0", ["max-visible"] = "5",
}
local switches = { icons = true, ["ignore-timeout"] = true }

-- What the file says, else mako's default; a switch's 0/1 as on/off.
local function now(key)
  local v = cfg:get(key, defaults[key])
  if switches[key] then return v == "1" end
  return v
end

page {
  group "Where" {
    choice { "anchor", label = "Corner", value = now("anchor"),
             choices = { { "top-right", "Top right" }, { "top-center", "Top" },
                         { "top-left", "Top left" }, { "bottom-right", "Bottom right" },
                         { "bottom-center", "Bottom" }, { "bottom-left", "Bottom left" },
                         { "center-right", "Right" }, { "center-left", "Left" },
                         { "center", "Middle" } } },
    choice { "layer", label = "Layer", value = now("layer"),
             choices = { { "top", "Above windows" }, { "overlay", "Above everything" },
                         { "bottom", "Below windows" }, { "background", "On the desktop" } },
             description = "Above everything includes fullscreen windows." },
  },
  group "Size" {
    number { "width", label = "Width", unit = "px", min = 50, max = 4000, value = now("width") },
    number { "height", label = "Height at most", unit = "px", min = 20, max = 4000,
             value = now("height") },
    text { "margin", label = "Margin", value = now("margin"), hint = "10",
           description = "Pixels around each notification: one value, or up to four (top, right, bottom, left), separated by commas." },
    text { "padding", label = "Padding", value = now("padding"), hint = "5" },
    number { "border-size", label = "Border", unit = "px", min = 0, max = 50,
             value = now("border-size") },
    text { "border-radius", label = "Corner radius", value = now("border-radius"), hint = "0" },
  },
  group "Look" {
    text { "font", label = "Font", value = now("font"), hint = "monospace 10",
           description = "A family and size, as Pango writes it." },
    color { "background-color", label = "Background", value = now("background-color") },
    color { "text-color", label = "Text", value = now("text-color") },
    color { "border-color", label = "Border", value = now("border-color") },
    switch { "icons", label = "Icons", value = now("icons") },
    number { "max-icon-size", label = "Icon size at most", unit = "px", min = 8, max = 512,
             value = now("max-icon-size") },
  },
  group "Timing" {
    number { "default-timeout", label = "Disappear after", unit = "ms", min = 0, max = 600000,
             value = now("default-timeout"),
             description = "0: they stay until dismissed." },
    switch { "ignore-timeout", label = "For every notification", value = now("ignore-timeout"),
             description = "Use this even when a program asks for another time." },
    number { "max-visible", label = "Shown at once", min = -1, max = 100,
             value = now("max-visible"), description = "-1: all of them." },
  },
  button { "Send a Test Notification", on_click = function()
    wb.run { "notify-send", "Waybox", "A test notification, as mako shows it now." }
  end },
  button { "Dismiss All", on_click = function()
    wb.run { "makoctl", "dismiss", "--all" }
  end },
}

function on_change(key, value)
  if key == "margin" or key == "padding" or key == "border-radius" then
    local ok = value == "" or value:match("^%d+$") or value:match("^%d+,%d+$") or
        value:match("^%d+,%d+,%d+$") or value:match("^%d+,%d+,%d+,%d+$")
    wb.problem(key, ok and "" or "Up to four numbers, separated by commas.")
  end
end

function on_save(values)
  for key, value in pairs(values) do
    if switches[key] then value = value == "on" and "1" or "0" end
    -- Only what's changed from mako's default, or already in the file.
    if cfg:get(key) ~= nil or value ~= defaults[key] then
      cfg:set(key, value)
    end
  end
  wb.write(path, cfg:text())
  wb.reload()
end
