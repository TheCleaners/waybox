#!/usr/bin/env python3
"""Integration test: the mako page Waybox ships (data/integrations/mako.lua),
through wbconf-lua, with wb.ini underneath.

1. No config file: mako's defaults shown.
2. A config with comments, a value with '#', and criteria sections: its
   values shown (a switch from 0/1).
3. A margin that isn't up to four numbers: a problem shown.
4. Save, with the width and icons changed: the file written back with only
   those lines changed (comments, spacing and sections as they were), a key
   left at its default not added, and mako reloaded.

Usage: mako_page_test.py <waybox> <wbconf-lua> <integrations-dir>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

CONFIG = """# my notifications
font = Inter 11
background-color=#1e1e2eee
icons=0

[urgency=high]
border-color=#f38ba8
default-timeout=0
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
            elif "value" in c:
                out[c["key"]] = c["value"]
    walk(page["controls"])
    return out


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    runner, integ = sys.argv[2], sys.argv[3]
    work = tempfile.mkdtemp(prefix="waybox-makopage-", dir="/tmp")
    home = os.path.join(work, "home")
    conf = os.path.join(home, ".config", "mako", "config")
    os.makedirs(os.path.dirname(conf))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    opened = {"open": {"script": os.path.join(integ, "mako.lua"), "home": home,
                       "files": ["~/.config/mako/config"], "runs": ["makoctl", "notify-send"]}}
    failures = []

    def run(msgs):
        p = subprocess.Popen([runner], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=env)
        try:
            return [ask(p, m) for m in msgs]
        finally:
            p.stdin.close()
            p.wait(timeout=10)

    # 1. Defaults.
    (r,) = run([opened])
    pages = [m["page"] for m in r if "page" in m]
    if not pages:
        failures.append(f"no page: {r}")
    else:
        v = values(pages[0])
        want = {"anchor": "top-right", "width": "300", "font": "monospace 10",
                "icons": "on", "default-timeout": "0", "background-color": "#285577FF"}
        for k, x in want.items():
            if v.get(k) != x:
                failures.append(f"default {k}: {v.get(k)!r}, not {x!r}")

    # 2-4. A real config.
    with open(conf, "w") as f:
        f.write(CONFIG)
    r_open, r_bad, r_save = run([
        opened,
        {"change": {"key": "margin", "value": "10,5,x"}},
        {"save": {"width": "420", "icons": "on", "margin": "10", "anchor": "top-right"}},
    ])
    pages = [m["page"] for m in r_open if "page" in m]
    if not pages:
        failures.append(f"no page from the config: {r_open}")
    else:
        v = values(pages[0])
        if v.get("font") != "Inter 11" or v.get("background-color") != "#1e1e2eee" or \
                v.get("icons") != "":
            failures.append(f"from the file: {v}")
    if r_bad != [{"update": {"margin": {"problem": "Up to four numbers, separated by commas."}}}]:
        failures.append(f"a bad margin: {r_bad}")
    writes = [m["write"] for m in r_save if "write" in m]
    runs = [m["run"] for m in r_save if "run" in m]
    if len(writes) != 1:
        failures.append(f"save: {r_save}")
    else:
        text = writes[0]["text"]
        want = CONFIG.replace("icons=0\n", "icons=1\nwidth=420\n")
        if text != want:
            failures.append(f"the file as written:\n{text}\nnot:\n{want}")
    if runs != [["makoctl", "reload"]]:
        failures.append(f"runs on save: {runs}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration mako page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
