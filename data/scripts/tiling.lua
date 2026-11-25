-- Tiling, per desktop: Super+Alt+T turns it on or off for the desktop
-- shown (off everywhere to begin with: a desktop not tiled floats, as in
-- Openbox). One big window on the left (the most recently focused), the
-- others stacked on the right. Maximized, fullscreen, minimized windows
-- and dialogs stay out of it. Enable it in rc.xml:
--   <waybox><scripts><script>/usr/share/waybox/scripts/tiling.lua</script></scripts></waybox>

local key = "W-A-t"
local main_share = 0.55   -- of the width, for the main window
local gap = 6

local tiled = {}          -- desktop number -> true

wb.policy("layout", function(windows, area, output, desktop)
  if not tiled[desktop] or #windows == 0 then return nil end
  -- The most recently focused first.
  local order = {}
  for _, w in ipairs(wb.windows()) do
    for _, v in ipairs(windows) do
      if v == w then table.insert(order, v) end
    end
  end
  local out = {}
  local x, y, w, h = area.x + gap, area.y + gap, area.width - 2 * gap, area.height - 2 * gap
  if #order == 1 then
    out[order[1].id] = { x = x, y = y, width = w, height = h }
    return out
  end
  local mw = math.floor(w * main_share)
  out[order[1].id] = { x = x, y = y, width = mw - gap, height = h }
  local n = #order - 1
  local sh = math.floor((h - (n - 1) * gap) / n)
  for i = 2, #order do
    out[order[i].id] = {
      x = x + mw, y = y + (i - 2) * (sh + gap), width = w - mw, height = sh,
    }
  end
  return out
end)

-- The focused window becomes the main one.
wb.on("window.focus", function() wb.relayout() end)

wb.bind(key, function()
  local d = wb.desktop().current
  tiled[d] = not tiled[d] or nil
  wb.log("tiling on desktop " .. d .. ": " .. (tiled[d] and "on" or "off"))
  wb.relayout()
end)
