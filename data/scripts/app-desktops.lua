-- Programs open on the desktop of your choice (by app id; see
-- "waybox-msg get_windows" for them). Copy this file to ~/.config/waybox/,
-- edit the table, and list it in rc.xml:
--   <waybox><scripts><script>app-desktops.lua</script></scripts></waybox>

local desktops = {
  ["firefox"] = 2,
  ["thunderbird"] = 3,
  ["org.gnome.Nautilus"] = 1,
}

wb.on("window.new", function(w)
  local d = desktops[w.app_id or ""]
  if d and w.desktop ~= d then w:move_to_desktop(d) end
end)
