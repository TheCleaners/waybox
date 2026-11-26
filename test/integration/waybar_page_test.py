#!/usr/bin/env python3
"""Integration test: the Waybar page Waybox ships (data/integrations/waybar.lua),
through wbconf-lua, with wb.json underneath.

1. No config of the user's (nor a default to read): Waybar's defaults, and
   a note that saving makes the user's own copy; saved, only what changed
   is written, to ~/.config/waybar/config.jsonc.
2. A config with comments and a trailing comma: its values shown, lists as
   "a, b"; a margin that isn't up to four numbers: a problem; the clock's
   format hidden when no list has the clock, shown again when one does.
3. Save, with the height, the right modules and the clock's format
   changed: only those values changed in the file (comments and the rest
   as they were), a setting left at its default not added, and Waybar
   reloaded (wb.reload()).
4. Two bars (an array): a note that the first is set; it is.

Usage: waybar_page_test.py <waybox> <wbconf-lua> <integrations-dir>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

CONFIG = """// my bar
{
    "height": 30, // a bit taller
    "spacing": 4,
    "modules-left": ["sway/workspaces"],
    "modules-center": [],
    "modules-right": ["tray", "clock"],
    "clock": {
        "format": "{:%H:%M}",
    },
}
"""

SAVED = """// my bar
{
    "height": 36, // a bit taller
    "spacing": 4,
    "modules-left": ["sway/workspaces"],
    "modules-center": [],
    "modules-right": ["pulseaudio", "tray", "clock"],
    "clock": {
        "format": "{:%a %H:%M}",
    },
}
"""

TWO = """[
    { "height": 20 },
    { "height": 40 },
]
"""


def ask(p, msg):
    p.stdin.write(json.dumps(msg) + "\n")
    p.stdin.flush()
    out = []
    while True:
        line = p.stdout.readline()
        if not line:
            return out + [{"eof": True}]
        m = json.loads(line)
        if m.get("done"):
            return out
        out.append(m)


def values(page):
    out = {}

    def walk(cs):
        for c in cs:
            if c["kind"] == "group":
                walk(c.get("children", []))
            elif "value" in c and c.get("key"):
                out[c["key"]] = c["value"]
    walk(page["controls"])
    return out


def labels(page):
    return [c.get("label", "") for c in page["controls"] if c["kind"] == "label"]


def errors(replies):
    return [m["error"] for m in replies if "error" in m]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    runner, integ = sys.argv[2], sys.argv[3]
    work = tempfile.mkdtemp(prefix="waybox-waybarpage-", dir="/tmp")
    home = os.path.join(work, "home")
    conf = os.path.join(home, ".config", "waybar", "config.jsonc")
    os.makedirs(os.path.dirname(conf))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    # Not the system's default: the page goes on without it.
    opened = {"open": {"script": os.path.join(integ, "waybar.lua"), "home": home,
                       "files": ["~/.config/waybar/config.jsonc", "~/.config/waybar/config"],
                       "runs": []}}
    failures = []

    def run(msgs):
        p = subprocess.Popen([runner], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=env)
        try:
            return [ask(p, m) for m in msgs]
        finally:
            p.stdin.close()
            p.wait(timeout=10)

    # 1. Nothing of the user's.
    r_open, r_save = run([opened, {"save": {"position": "bottom", "layer": "bottom",
                                            "height": "0", "spacing": "0", "margin": "",
                                            "output": "", "exclusive": "on",
                                            "modules-left": "", "modules-center": "",
                                            "modules-right": "clock", "clock-format": ""}}])
    pages = [m["page"] for m in r_open if "page" in m]
    if not pages or errors(r_open):
        failures.append(f"no page without a config: {r_open}")
    else:
        v = values(pages[0])
        if v.get("position") != "top" or v.get("height") != "0" or v.get("exclusive") != "on":
            failures.append(f"Waybar's defaults: {v}")
        if not any("makes your own copy" in l for l in labels(pages[0])):
            failures.append(f"no note about the copy: {labels(pages[0])}")
    writes = [m["write"] for m in r_save if "write" in m]
    want = '{\n    "position": "bottom",\n    "modules-right": ["clock"]\n}\n'
    if len(writes) != 1 or writes[0]["path"] != conf or writes[0]["text"] != want:
        failures.append(f"saved without a config: {r_save}")

    # 2-3. A real config.
    with open(conf, "w") as f:
        f.write(CONFIG)
    r_open, r_bad, r_none, r_back, r_save = run([
        opened,
        {"change": {"key": "margin", "value": "4 x"}},
        {"change": {"key": "modules-right", "value": "tray"}},
        {"change": {"key": "modules-right", "value": "tray, clock"}},
        {"save": {"position": "top", "layer": "bottom", "height": "36", "spacing": "4",
                  "margin": "", "output": "", "exclusive": "on",
                  "modules-left": "sway/workspaces", "modules-center": "",
                  "modules-right": "pulseaudio, tray, clock", "clock-format": "{:%a %H:%M}"}},
    ])
    pages = [m["page"] for m in r_open if "page" in m]
    if not pages or errors(r_open):
        failures.append(f"no page from the config: {r_open}")
    else:
        v = values(pages[0])
        if v.get("height") != "30" or v.get("modules-right") != "tray, clock" or \
                v.get("clock-format") != "{:%H:%M}" or v.get("modules-center") != "":
            failures.append(f"the config's values: {v}")
        if labels(pages[0]):
            failures.append(f"notes with a config of one's own: {labels(pages[0])}")
    if r_bad != [{"update": {"margin": {"problem": "Up to four numbers, separated by spaces."}}}]:
        failures.append(f"a bad margin: {r_bad}")
    if r_none != [{"update": {"clock-format": {"visible": False}}}]:
        failures.append(f"the clock's format not hidden: {r_none}")
    if r_back != [{"update": {"clock-format": {"visible": True}}}]:
        failures.append(f"the clock's format not shown again: {r_back}")
    writes = [m["write"] for m in r_save if "write" in m]
    if errors(r_save) or len(writes) != 1 or writes[0]["text"] != SAVED:
        failures.append(f"save: {r_save}")
    if [m for m in r_save if "reload" in m] != [{"reload": True}]:
        failures.append(f"Waybar not reloaded: {r_save}")

    # 4. Two bars.
    with open(conf, "w") as f:
        f.write(TWO)
    (r_open,) = run([opened])
    pages = [m["page"] for m in r_open if "page" in m]
    if not pages or errors(r_open):
        failures.append(f"no page for two bars: {r_open}")
    else:
        if values(pages[0]).get("height") != "20":
            failures.append(f"not the first bar: {values(pages[0])}")
        if not any("2 bars" in l for l in labels(pages[0])):
            failures.append(f"no note about the bars: {labels(pages[0])}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration waybar page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
