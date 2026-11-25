-- A drop-down terminal: Super+grave shows it (starting it the first time)
-- or hides it. Enable it in rc.xml:
--   <waybox><scripts><script>/usr/share/waybox/scripts/scratchpad.lua</script></scripts></waybox>
-- (or copy it to ~/.config/waybox/ and change the settings below).

local key = "W-grave"
local app_id = "scratchpad"
-- A terminal that can set its app id: foot -a, kitty --class, alacritty --class.
local command = { "foot", "-a", app_id }
local geometry = { width = 1000, height = 500 }  -- centred near the top

local function find()
  for _, w in ipairs(wb.windows()) do
    if w.app_id == app_id then return w end
  end
end

wb.on("window.new", function(w)
  if w.app_id ~= app_id then return end
  local out = wb.outputs()[1]
  if out then
    w:set_geometry{
      x = out.x + math.floor((out.width - geometry.width) / 2), y = out.y + 40,
      width = geometry.width, height = geometry.height,
    }
  end
  if not w.omnipresent then w:toggle_omnipresent() end
end)

wb.bind(key, function()
  local w = find()
  if w == nil then
    wb.spawn(command)
  elseif w.focused and not w.minimized then
    w:minimize()
  else
    w:focus()  -- brings it back if minimized
  end
end)
