#!/usr/bin/env python3
"""Integration test: the fuzzel page Waybox ships (data/integrations/fuzzel.lua),
through wbconf-lua, with wb.ini underneath.

1. No config file: fuzzel's defaults shown (its colors with '#').
2. A config with its main keys before any section, [colors] and [border],
   comments and a quoted prompt: its values shown (the prompt unquoted,
   the colors with '#'); a line height that isn't a number: a problem.
3. Save, with the font, width, a color (#rrggbb), the icons and the corner
   radius (emptied) changed, and that line height: the file written back
   with only those lines changed (the prompt still quoted, the color as
   RRGGBBAA without '#', the radius's line gone, the bad line height not
   written, new keys in order), comments and the rest as they were.

Usage: fuzzel_page_test.py <waybox> <wbconf-lua> <integrations-dir>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

CONFIG = """# my launcher
font=Iosevka:size=13
prompt="❯ "
lines=10

[colors]
background=1e1e2eee
# a comment
selection=313244ff

[border]
radius=0
"""

SAVED = """# my launcher
font=Iosevka:size=14:weight=bold
prompt="❯ "
lines=10
icons-enabled=no
width=40

[colors]
background=1e1e2eee
# a comment
selection=313244ff
text=cdd6f4ff

[border]
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


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    runner, integ = sys.argv[2], sys.argv[3]
    work = tempfile.mkdtemp(prefix="waybox-fuzzelpage-", dir="/tmp")
    home = os.path.join(work, "home")
    conf = os.path.join(home, ".config", "fuzzel", "fuzzel.ini")
    os.makedirs(os.path.dirname(conf))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    opened = {"open": {"script": os.path.join(integ, "fuzzel.lua"), "home": home,
                       "files": ["~/.config/fuzzel/fuzzel.ini"], "runs": []}}
    failures = []

    def run(msgs):
        p = subprocess.Popen([runner], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=env)
        try:
            return [ask(p, m) for m in msgs]
        finally:
            if p.stdin:
                p.stdin.close()
            p.wait(timeout=10)

    # 1. Defaults.
    (r,) = run([opened])
    pages = [m["page"] for m in r if "page" in m]
    if not pages:
        failures.append(f"no page: {r}")
    else:
        v = values(pages[0])
        want = {"font": "monospace", "anchor": "center", "lines": "15", "prompt": "> ",
                "icons-enabled": "on", "color-background": "#fdf6e3ff",
                "border-radius": "10", "match-mode": "fzf"}
        for k, x in want.items():
            if v.get(k) != x:
                failures.append(f"default {k}: {v.get(k)!r}, not {x!r}")

    # 2-3. A real config.
    with open(conf, "w") as f:
        f.write(CONFIG)
    r_open, r_bad, r_save = run([
        opened,
        {"change": {"key": "line-height", "value": "20x"}},
        {"save": {"font": "Iosevka:size=14:weight=bold", "width": "40",
                  "color-text": "#cdd6f4", "border-radius": "", "icons-enabled": "",
                  "line-height": "20x"}},
    ])
    pages = [m["page"] for m in r_open if "page" in m]
    if not pages:
        failures.append(f"no page from the config: {r_open}")
    else:
        v = values(pages[0])
        want = {"font": "Iosevka:size=13", "prompt": "❯ ", "lines": "10",
                "color-background": "#1e1e2eee", "color-selection": "#313244ff",
                "border-radius": "0"}
        for k, x in want.items():
            if v.get(k) != x:
                failures.append(f"from the file, {k}: {v.get(k)!r}, not {x!r}")
        kinds = {c.get("key"): c for g in pages[0]["controls"] if g["kind"] == "group"
                 for c in g.get("children", [])}
        if kinds.get("font", {}).get("kind") != "font" or \
                kinds.get("font", {}).get("format") != "fontconfig":
            failures.append(f"the font control: {kinds.get('font')}")
    if r_bad != [{"update": {"line-height": {
            "problem": "A number, with pt or px after it if you like."}}}]:
        failures.append(f"a bad line height: {r_bad}")
    writes = [m["write"] for m in r_save if "write" in m]
    if len(writes) != 1 or writes[0]["text"] != SAVED:
        failures.append(f"saved:\n{writes[0]['text'] if writes else r_save}\nnot:\n{SAVED}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration fuzzel page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
