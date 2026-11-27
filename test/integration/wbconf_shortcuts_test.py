#!/usr/bin/env python3
"""Integration test: wbconf's Shortcuts page lists rc.xml's bindings.

A chain (C-a then x), an Execute with Openbox's older <execute>, a binding
with If (advanced: left to rc.xml), and mouse bindings in two contexts:
each listed with what it does, under its category.

Then edited through the page's widgets and saved: keys moved (W-d to
W-S-d), a command changed (where it stands, as <command>), an action
swapped (its options dropped), a chain removed, one added; rc.xml has
exactly that, the If binding and the comment untouched.

Then the mouse and search: a binding given Alt and moved to the window, one
moved to the wheel (its event becomes Click), one added; searching
"maximize" shows only the bindings that maximize and offers the actions no
key runs yet; picking one adds a shortcut for it.
Skips (exit 77) without a headless display.

Usage: wbconf_shortcuts_test.py <waybox> <wbconf> <schema-dir>
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
  <keyboard>
    <!-- mine -->
    <keybind key="W-d"><action name="ToggleShowDesktop"/></keybind>
    <keybind key="W-Return"><action name="Execute"><execute>foot</execute></action></keybind>
    <keybind key="C-a"><keybind key="x"><action name="Close"/></keybind></keybind>
    <keybind key="W-i"><action name="If"><then><action name="Raise"/></then></action></keybind>
    <keybind key="W-F2"><action name="GoToDesktop"><to>2</to></action></keybind>
  </keyboard>
  <mouse>
    <context name="Root">
      <mousebind button="Right" action="Press"><action name="ShowMenu"><menu>root-menu</menu></action></mousebind>
    </context>
    <context name="Titlebar">
      <mousebind button="Left" action="DoubleClick"><action name="ToggleMaximize"/></mousebind>
    </context>
  </mouse>
</openbox_config>
"""

WANT = [
    "key|W-d|Show the desktop|Desktops",
    "key|W-Return|Run foot|Programs and menus",
    "key|C-a x|Close the window|Windows",
    "key|W-i|If (edit in rc.xml)|Other",
    "key|W-F2|Go to desktop 2|Desktops",
    "mouse|Root Right Press|Show the root menu|Programs and menus",
    "mouse|Titlebar Left DoubleClick|Maximize or restore|Windows",
]


EDITS = ";".join([
    "shortcut-keys:W-d=W-S-d",
    "shortcut-option:W-Return=command=alacritty",
    "shortcut-action:W-F2=Close",
    "shortcut-remove:C-a x",
    "shortcut-add:W-n=ToggleMaximize",
    "shortcuts", "changes", "save", "status", "shortcuts"])


MOUSE_EDITS = ";".join([
    "shortcut-mod:Titlebar Left DoubleClick=A-",
    "shortcut-place:Titlebar A-Left DoubleClick=Client",
    "shortcut-button:Root Right Press=Up",
    "shortcut-addmouse:Icon Middle Click=Close",
    "search:maximize", "shortcut-visible",
    "shortcut-suggest:Maximize", "shortcut-keys:=W-m",
    "search:", "shortcut-visible", "changes", "save", "status"])


def mousebinds(path):
    """rc.xml's mouse bindings: {"Root Right Press": [(action, options)]}."""
    import xml.etree.ElementTree as ET
    ns = "{http://openbox.org/3.4/rc}"
    out = {}
    mouse = ET.parse(path).getroot().find(ns + "mouse")
    for ctx in mouse.findall(ns + "context") if mouse is not None else []:
        for mb in ctx.findall(ns + "mousebind"):
            acts = [(a.get("name"), {c.tag[len(ns):]: (c.text or "") for c in a})
                    for a in mb.findall(ns + "action")]
            out[" ".join([ctx.get("name"), mb.get("button"), mb.get("action")])] = acts
    return out


def keybinds(path):
    """rc.xml's key bindings: {"W-d": [(action, {option: value})], ...},
    a chain's keys joined by a space."""
    import xml.etree.ElementTree as ET
    ns = "{http://openbox.org/3.4/rc}"
    out = {}

    def walk(node, chain):
        for kb in node.findall(ns + "keybind"):
            keys = chain + [kb.get("key")]
            acts = [(a.get("name"), {c.tag[len(ns):]: (c.text or "") for c in a})
                    for a in kb.findall(ns + "action")]
            if acts:
                out[" ".join(keys)] = acts
            walk(kb, keys)
    walk(ET.parse(path).getroot().find(ns + "keyboard"), [])
    return out


def run_wbconf(wbconf, wenv, steps, wait):
    app = subprocess.Popen([wbconf], env=dict(wenv, WBCONF_TEST_TOC=steps),
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(wait)
    app.kill()
    return app.communicate()[1]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-shortcuts-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
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
    err = run_wbconf(wbconf, wenv, "click:Shortcuts;shortcuts", 0.8 * 4 + 1.5)
    got = [l[len("shortcut:"):] for l in err.splitlines() if l.startswith("shortcut:")]
    failures = []
    if got != WANT:
        failures.append("rows:\n  " + "\n  ".join(got) + "\nnot:\n  " + "\n  ".join(WANT))

    # Edited, then saved.
    rc = os.path.join(cfg, "waybox", "rc.xml")
    err = run_wbconf(wbconf, wenv, "click:Shortcuts;" + EDITS, 0.8 * 14 + 2)
    lines = err.splitlines()
    cut = next((i for i, l in enumerate(lines) if l.startswith("changes-dump:")), len(lines))

    def keyrows(part):
        return [l[len("shortcut:"):] for l in part if l.startswith("shortcut:key|")]
    before_save, after_save = keyrows(lines[:cut]), keyrows(lines[cut:])
    want_before = [
        "key|W-S-d|Show the desktop|Desktops|changed",
        "key|W-Return|Run alacritty|Programs and menus|changed",
        "key|C-a x|Close the window|Windows|removed",
        "key|W-i|If (edit in rc.xml)|Other",
        "key|W-F2|Close the window|Windows|changed",
        "key|W-n|Maximize or restore|Windows|changed",
    ]
    if before_save != want_before:
        failures.append("edited rows:\n  " + "\n  ".join(before_save))
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if not changes or not changes[0].startswith("changes-dump:5|"):
        failures.append(f"unsaved changes: {changes}")
    if any(r.endswith(("|changed", "|removed")) for r in after_save) or len(after_save) != 5:
        failures.append("after saving:\n  " + "\n  ".join(after_save))
    got_rc = keybinds(rc)
    want_rc = {
        "W-S-d": [("ToggleShowDesktop", {})],
        # Edited in place; written as Openbox 3.4's <command>.
        "W-Return": [("Execute", {"command": "alacritty"})],
        "W-i": [("If", {"then": ""})],
        "W-F2": [("Close", {})],
        "W-n": [("ToggleMaximize", {})],
    }
    if got_rc != want_rc:
        failures.append(f"rc.xml bindings: {got_rc}")
    text = open(rc).read()
    if "<!-- mine -->" not in text:
        failures.append("the comment in <keyboard> was lost")
    if 'key="C-a"' in text:
        failures.append("the removed chain left its C-a behind")

    # The mouse, and search.
    err = run_wbconf(wbconf, wenv, "click:Shortcuts;" + MOUSE_EDITS, 0.8 * 16 + 2)
    lines = err.splitlines()
    visible = [l for l in lines if l.startswith("shortcut-visible:")]
    # The new mouse binding too: rows being set up stay in sight.
    want_visible = ("shortcut-visible:W-n,Client A-Left DoubleClick,Icon Middle Click"
                    "|suggest:Maximize,Unmaximize")
    if not visible or visible[0] != want_visible:
        failures.append(f"searching maximize: {visible[:1]}")
    if len(visible) < 2 or visible[1].count(",") < 8 or "|suggest:" not in visible[1] \
            or not visible[1].endswith("|suggest:"):
        failures.append(f"search cleared: {visible[1:2]}")
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if not changes or not changes[0].startswith("changes-dump:4|"):
        failures.append(f"unsaved changes: {changes}")
    got_mouse = mousebinds(rc)
    want_mouse = {
        "Client A-Left DoubleClick": [("ToggleMaximize", {})],
        "Root Up Click": [("ShowMenu", {"menu": "root-menu"})],
        "Icon Middle Click": [("Close", {})],
    }
    if got_mouse != want_mouse:
        failures.append(f"rc.xml mouse bindings: {got_mouse}")
    if keybinds(rc).get("W-m") != [("Maximize", {})]:
        failures.append(f"the suggested shortcut: {keybinds(rc).get('W-m')}")
    if "<context name=\"Titlebar\"" in open(rc).read():
        failures.append("the emptied Titlebar context was left behind")
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
    print("integration wbconf shortcuts: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
