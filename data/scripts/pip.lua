-- Picture-in-picture video windows (Firefox, Chromium) stay on every
-- desktop. Enable it in rc.xml:
--   <waybox><scripts><script>/usr/share/waybox/scripts/pip.lua</script></scripts></waybox>

local titles = { "Picture%-in%-Picture", "Picture in picture" }

local function is_pip(w)
  if w.tag == "pip" or w.tag == "picture-in-picture" then return true end
  for _, t in ipairs(titles) do
    if (w.title or ""):match(t) then return true end
  end
  return false
end

local function place(w)
  if is_pip(w) and not w.omnipresent then w:toggle_omnipresent() end
end

wb.on("window.new", place)
-- Some browsers set the title only after the window opens.
wb.on("window.title", place)
