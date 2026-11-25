-- A menu of the open windows, by desktop: choosing one goes to it. Super+W
-- shows it; to have it in the root menu too, add <menu id="lua-windows"/>
-- to menu.xml (or bind ShowMenu with <menu>lua-windows</menu>). Enable it
-- in rc.xml:
--   <waybox><scripts><script>/usr/share/waybox/scripts/windows-menu.lua</script></scripts></waybox>

local key = "W-w"
local id = "lua-windows"

local function items()
  local menu = {}
  local by_desktop = {}
  for _, w in ipairs(wb.windows()) do
    local d = w.omnipresent and 0 or w.desktop
    by_desktop[d] = by_desktop[d] or {}
    table.insert(by_desktop[d], w)
  end
  for d = 0, wb.desktop().count do
    local list = by_desktop[d]
    if list then
      table.insert(menu, { separator = true,
                           label = d == 0 and "All desktops" or ("Desktop " .. d) })
      for _, w in ipairs(list) do
        local label = (w.title ~= nil and w.title ~= "") and w.title or w.app_id
        if w.minimized then label = "(" .. label .. ")" end
        table.insert(menu, { label = label, icon = w.app_id,
                             action = function() w:focus() end })
      end
    end
  end
  return menu
end

wb.menu_provider(id, items, "Windows")
wb.bind(key, function()
  local menu = items()
  if #menu == 0 then
    wb.osd("No windows")
  else
    wb.show_menu(menu)
  end
end)
