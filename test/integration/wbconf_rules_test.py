#!/usr/bin/env python3
"""Integration test: wbconf lists rc.xml's window rules (#237).

In order (the first that matches wins), each with what it matches and
what it does; what Waybox doesn't read (role=, <shade>, a "center"
position) reported as kept as written. An rc.xml with none: an empty
list saying what rules are for.

Then edited through the editor's widgets: settings set and cleared, a rule
removed, one added, two moved; saved, rc.xml has exactly that in that
order, comments and what Waybox doesn't read kept.
Skips (exit 77) without a headless display.

Usage: wbconf_rules_test.py <waybox> <wbconf> <schema-dir>
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <application class="mpv"><fullscreen>yes</fullscreen></application>
    <application class="foot" title="*scratch*" role="x">
      <position><x>200</x><y>120</y></position>
      <size><width>900</width><height>600</height></size>
      <decor>no</decor>
      <shade>yes</shade>
    </application>
    <application class="gimp">
      <position><x>center</x></position>
      <desktop>all</desktop>
    </application>
    <application title="Picture-in-Picture"><layer>above</layer></application>
    <application match="regex" class="^(foot|alacritty)$"><desktop>2</desktop></application>
    <application match="iregex" title="(unclosed"><iconic>yes</iconic></application>
  </applications>
</openbox_config>
"""

WANT = [
    "rule|mpv||Full screen|",
    "rule|foot|*scratch*|At 200, 120;900 \u00d7 600;No title bar|role,shade",
    "rule|gimp||On every desktop;At 0, auto||position",
    "rule||Picture-in-Picture|Always on top|",
    "rule|^(foot|alacritty)$||On desktop 2||regex",
    "rule||(unclosed|Starts minimized||iregex|never",
]


EDITS = ";".join([
    "rule-set:1=desktop=all",
    "rule-set:2=decor=yes", "rule-set:2=width=",
    "rule-set:3=desktop=",
    "rule-remove:6",
    "rule-add", "rule-set:7=program=firefox", "rule-set:7=maximized=yes",
    "rule-later:1",       # mpv after foot
    "rule-earlier:7",     # firefox before the removed one
    "rules", "changes", "save", "status", "rules"])


def rules_in(path):
    """rc.xml's rules: (attributes, {setting: text}) in order."""
    import xml.etree.ElementTree as ET
    ns = "{http://openbox.org/3.4/rc}"
    out = []
    for a in ET.parse(path).getroot().find(ns + "applications").findall(ns + "application"):
        kids = {}
        for c in a:
            if len(c):
                kids[c.tag[len(ns):]] = {g.tag[len(ns):]: g.text for g in c}
            else:
                kids[c.tag[len(ns):]] = c.text
        out.append((dict(a.attrib), kids))
    return out


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-rules-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc, "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    sock = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    if sock is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid))

    def run(steps, wait=0.8 * 3 + 1.5, all_lines=False):
        app = subprocess.Popen([wbconf], env=dict(wenv, WBCONF_TEST_TOC=steps),
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        time.sleep(wait)
        app.kill()
        lines = app.communicate()[1].splitlines()
        return lines if all_lines else [l for l in lines if l.startswith("rule|")]

    failures = []
    got = run("dump;rules")
    if got != WANT:
        failures.append("rules:\n  " + "\n  ".join(got) + "\nnot:\n  " + "\n  ".join(WANT))
    # Edited, then saved.
    lines = run(EDITS, 0.8 * 16 + 2, all_lines=True)
    cut = next((i for i, l in enumerate(lines) if l.startswith("changes-dump:")), len(lines))
    before = [l for l in lines[:cut] if l.startswith("rule|")]
    after = [l for l in lines[cut:] if l.startswith("rule|")]
    want_before = [
        "rule|foot|*scratch*|At 200, 120;auto \u00d7 600;With title bar|role,shade|changed",
        "rule|mpv||On every desktop;Full screen||changed",
        "rule|gimp||At 0, auto||position|changed",
        "rule||Picture-in-Picture|Always on top|",
        "rule|^(foot|alacritty)$||On desktop 2||regex",
        "rule|firefox||Maximized||changed",
        "rule||(unclosed|Starts minimized||iregex|never|removed",
    ]
    if before != want_before:
        failures.append("edited:\n  " + "\n  ".join(before))
    ch = [l for l in lines if l.startswith("changes-dump:")]
    if not ch or not ch[0].startswith("changes-dump:6|"):
        failures.append(f"unsaved changes (5 rules and the order): {ch[:1]}")
    if len(after) != 6 or any(l.endswith(("|changed", "|removed")) for l in after):
        failures.append("after saving:\n  " + "\n  ".join(after))
    got = rules_in(rc)
    want = [
        ({"class": "foot", "title": "*scratch*", "role": "x"},
         {"position": {"x": "200", "y": "120"}, "size": {"height": "600"}, "decor": "yes",
          "shade": "yes"}),
        ({"class": "mpv"}, {"fullscreen": "yes", "desktop": "all"}),
        ({"class": "gimp"}, {"position": {"x": "center"}}),
        ({"title": "Picture-in-Picture"}, {"layer": "above"}),
        ({"match": "regex", "class": "^(foot|alacritty)$"}, {"desktop": "2"}),
        ({"class": "firefox"}, {"maximized": "yes"}),
    ]
    if got != want:
        failures.append(f"rc.xml rules: {got}")

    with open(rc, "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"></openbox_config>\n')
    got = run("dump;rules")
    if got:
        failures.append(f"none in rc.xml, yet: {got}")
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf rules: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
