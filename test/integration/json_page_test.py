#!/usr/bin/env python3
"""Integration test: wb.json in Lua pages, driven over wbconf-lua's protocol.

A page reads a Waybar-style config (comments, a trailing comma, a top-level
array of bars) with wb.json.read and saves changes with cfg:set:
1. Values read through paths: { 1, "height" }, { 1, "clock", "format" };
   arrays come back as Lua arrays.
2. Saved: the changed values in place, a new key in the file's style, a
   removed one gone with its line; comments and everything else as it was.
3. Mistakes said: a file that isn't JSON (with where), a value JSON can't
   hold, setting through a number.

Usage: json_page_test.py <waybox> <wbconf-lua>   (waybox: unused)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CONFIG = """// Waybar
[
    {
        "layer": "top", // on top
        "height": 30,
        "modules-left": ["sway/workspaces", "sway/mode"],
        "clock": {
            "format": "{:%H:%M}",
        },
        "spacing": 4,
    },
]
"""

SAVED = """// Waybar
[
    {
        "layer": "top", // on top
        "height": 36,
        "modules-left": ["sway/workspaces"],
        "clock": {
            "format": "{:%a %H:%M}",
            "interval": 60,
        },
    },
]
"""

PAGE = r'''
local cfg = wb.json.read("~/.config/waybar/config.jsonc")
local bar = cfg:get(1, {})
local mods = cfg:get({ 1, "modules-left" }, {})

page {
  number { "height", label = "Height", value = cfg:get({ 1, "height" }, 30) },
  text { "format", label = "Clock", value = cfg:get({ 1, "clock", "format" }, "") },
  text { "mods", label = "Left", value = #mods .. ":" .. table.concat(mods, ",") .. ":" ..
         tostring(bar.layer) },
  button { "Odd", key = "odd", on_click = function() cfg:set({ 1, "x" }, print) end },
  button { "Through", key = "through", on_click = function() cfg:set({ 1, "height", "x" }, 1) end },
  button { "Broken", key = "broken", on_click = function() wb.json.parse("{\n  \"a\": 1\n  \"b\": 2\n}", "broken.json") end },
}

function on_save(values)
  cfg:set({ 1, "height" }, tonumber(values.height))
  cfg:set_all({ format = values.format, interval = 60 }, { 1, "clock" })
  cfg:set({ 1, "modules-left" }, { "sway/workspaces" })
  cfg:set({ 1, "spacing" }, nil)
  wb.write("~/.config/waybar/config.jsonc", cfg:text())
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
    work = tempfile.mkdtemp(prefix="waybox-jsonpage-", dir="/tmp")
    home = os.path.join(work, "home")
    os.makedirs(os.path.join(home, ".config", "waybar"))
    path = os.path.join(home, ".config", "waybar", "config.jsonc")
    with open(path, "w") as f:
        f.write(CONFIG)
    script = os.path.join(work, "waybar.lua")
    with open(script, "w") as f:
        f.write(PAGE)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    failures = []
    r = Runner(exe, env)
    try:
        replies = r.ask({"open": {"script": script, "home": home,
                                  "files": ["~/.config/waybar/config.jsonc"], "runs": []}})
        pages = [m["page"] for m in replies if "page" in m]
        if len(pages) != 1 or errors(replies):
            failures.append(f"open: {replies}")
        else:
            values = {c["key"]: c.get("value") for c in pages[0]["controls"] if "key" in c}
            if values.get("height") != "30" or values.get("format") != "{:%H:%M}" or \
                    values.get("mods") != "2:sway/workspaces,sway/mode:top":
                failures.append(f"read: {values}")
        replies = r.ask({"save": {"height": "36", "format": "{:%a %H:%M}", "mods": ""}})
        writes = [m["write"] for m in replies if "write" in m]
        if errors(replies) or len(writes) != 1 or writes[0]["text"] != SAVED:
            failures.append(f"save: {replies}")
        e = errors(r.ask({"click": "odd"}))
        if not e or "can't be written as JSON" not in e[0]:
            failures.append(f"a function as a value: {e}")
        e = errors(r.ask({"click": "through"}))
        if not e or "can't set 1.height.x" not in e[0]:
            failures.append(f"setting through a number: {e}")
        e = errors(r.ask({"click": "broken"}))
        if not e or "broken.json: line 3" not in e[0]:
            failures.append(f"a file that isn't JSON: {e}")
    finally:
        r.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration json page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
