#!/usr/bin/env python3
"""Integration test: wb.toml in Lua pages, driven over wbconf-lua's protocol.

A page reads an Alacritty-style config (comments, tables, a dotted key, an
inline table, a float) with wb.toml.read and saves changes with cfg:set:
1. Values read through paths however the file spells them.
2. Saved: values changed in place (a float stays one, the comment after
   it kept), the inline table's members, a new table under its own header;
   everything else as it was.
3. Mistakes said: a file that isn't TOML (with where), null (TOML has
   none), setting through a value.

Usage: toml_page_test.py <waybox> <wbconf-lua>   (waybox: unused)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CONFIG = """# Alacritty
[window]
opacity = 0.9   # a bit see-through
padding = { x = 4, y = 4 }

[font]
size = 11.0
normal.family = "Iosevka"
"""

SAVED = """# Alacritty
[window]
opacity = 0.8   # a bit see-through
padding = { x = 8, y = 8 }

[font]
size = 13.0
normal.family = "Fira Code"

[cursor.style]
shape = "Beam"
"""

PAGE = r'''
local cfg = wb.toml.read("~/.config/alacritty/alacritty.toml")

page {
  number { "size", label = "Size", value = cfg:get({ "font", "size" }, 11) },
  text { "family", label = "Font", value = cfg:get({ "font", "normal", "family" }, "") },
  number { "padding", label = "Padding", value = cfg:get({ "window", "padding", "x" }, 0) },
  button { "Null", key = "null", on_click = function() cfg:set({ "a" }, { 1, wb.json.null }) end },
  button { "Through", key = "through", on_click = function() cfg:set({ "font", "size", "x" }, 1) end },
  button { "Broken", key = "broken", on_click = function() wb.toml.parse("a = 1\nb = \n", "broken.toml") end },
}

function on_save(values)
  cfg:set({ "font", "size" }, tonumber(values.size))
  cfg:set({ "font", "normal", "family" }, values.family)
  cfg:set_all({ x = tonumber(values.padding), y = tonumber(values.padding) }, { "window", "padding" })
  cfg:set({ "window", "opacity" }, 0.8)
  cfg:set({ "cursor", "style", "shape" }, "Beam")
  wb.write("~/.config/alacritty/alacritty.toml", cfg:text())
end
'''


class Runner:
    def __init__(self, exe, env):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env)

    def ask(self, msg, timeout=5.0):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        out = []
        end = time.time() + timeout
        while time.time() < end:
            line = self.p.stdout.readline()
            if not line:
                break
            m = json.loads(line)
            if m.get("done"):
                return out
            out.append(m)
        out.append({"timeout": True})
        return out

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.p.kill()


def errors(replies):
    return [r["error"]["message"] for r in replies if "error" in r]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    exe = sys.argv[2]
    work = tempfile.mkdtemp(prefix="waybox-tomlpage-", dir="/tmp")
    home = os.path.join(work, "home")
    os.makedirs(os.path.join(home, ".config", "alacritty"))
    path = os.path.join(home, ".config", "alacritty", "alacritty.toml")
    with open(path, "w") as f:
        f.write(CONFIG)
    script = os.path.join(work, "alacritty.lua")
    with open(script, "w") as f:
        f.write(PAGE)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    failures = []
    r = Runner(exe, env)
    try:
        replies = r.ask({"open": {"script": script, "home": home,
                                  "files": ["~/.config/alacritty/alacritty.toml"], "runs": []}})
        pages = [m["page"] for m in replies if "page" in m]
        if len(pages) != 1 or errors(replies):
            failures.append(f"open: {replies}")
        else:
            values = {c["key"]: c.get("value") for c in pages[0]["controls"] if "key" in c}
            if values.get("size") != "11" or values.get("family") != "Iosevka" or \
                    values.get("padding") != "4":
                failures.append(f"read: {values}")
        replies = r.ask({"save": {"size": "13", "family": "Fira Code", "padding": "8"}})
        writes = [m["write"] for m in replies if "write" in m]
        if errors(replies) or len(writes) != 1 or writes[0]["text"] != SAVED:
            failures.append(f"save: {replies}")
        e = errors(r.ask({"click": "null"}))
        if not e or "can't set a" not in e[0]:
            failures.append(f"null: {e}")
        e = errors(r.ask({"click": "through"}))
        if not e or "can't set font.size.x" not in e[0]:
            failures.append(f"setting through a value: {e}")
        e = errors(r.ask({"click": "broken"}))
        if not e or "broken.toml: line 2" not in e[0]:
            failures.append(f"a file that isn't TOML: {e}")
    finally:
        r.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration toml page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
