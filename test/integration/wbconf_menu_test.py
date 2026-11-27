#!/usr/bin/env python3
"""Integration test: wbconf's menu editor (#238).

menu.xml's root menu shown as Waybox reads it (items, a divider, a
submenu with its entries, a pipe menu, a menu defined elsewhere); then
changed through the page's widgets: an item relabeled and its command
changed, the pipe menu removed, a program, a submenu and a titled
separator added, entries moved into the submenu above, out again and
earlier, the last change undone; a program added from the installed ones
(its name, icon and command, field codes left out), and its icon chosen
from the theme. Saved: menu.xml has exactly that, its comment kept.
Skips (exit 77) without a headless display.

Usage: wbconf_menu_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

MENU = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
<!-- mine -->
<menu id="root-menu" label="Waybox">
  <item label="Terminal" icon="utilities-terminal">
    <action name="Execute"><execute>foot</execute></action>
  </item>
  <separator/>
  <menu id="tools" label="Tools">
    <item label="Calculator"><action name="Execute"><command>gnome-calculator</command></action></item>
  </menu>
  <menu id="recent" label="Recent" execute="recent-menu"/>
  <menu id="places"/>
  <item label="Exit"><action name="Exit"/></item>
</menu>
<menu id="places" label="Places">
  <item label="Home"><action name="Execute"><command>xdg-open ~</command></action></item>
</menu>
</openbox_menu>
"""

WANT_LOADED = [
    "menu|1|Terminal|utilities-terminal|Run foot",
    "menu|2|||A divider",
    "menu|3|Tools||A submenu of 1 entry",
    "menu|3.1|Calculator||Run gnome-calculator",
    "menu|4|Recent||Entries from recent-menu",
    "menu|5|||Shows the menu \u201cPlaces\u201d",
    "menu|6|Exit||Log out",
]

STEPS = ";".join([
    "click:Menu", "menu",
    "menu-set:1=label=Console", "menu-option:1=command=alacritty",
    "menu-remove:4",                 # the pipe menu
    "menu-add:item", "menu-set:6=label=Editor", "menu-option:6=command=gedit",
    "menu-into:6",                   # Editor is after Exit (5): not into; still 6
    "menu-earlier:6",                # Editor before Exit: 5
    "menu-earlier:5",                # before the reference: 4
    "menu-into:4",                   # into Tools (3), at its end: 3.2
    "menu-add:submenu", "menu-set:6=label=Games",
    "menu-add:separator", "menu-set:7=label=Later",
    "menu-undo",                     # the separator's title
    "menu-undo",                     # the separator
    "menu-out:3.2",                  # Editor out of Tools, after it: 4
    "menu-addapp:Test Editor",       # from the installed programs: 8
    "menu-iconpick:8=folder",        # an icon from the theme's (if it has one)
    "menu", "changes", "save", "status", "menu"])


def entries(path):
    """menu.xml's root menu: (tag, attributes, [children], command)."""
    import xml.etree.ElementTree as ET
    ns = "{http://openbox.org/3.4/menu}"
    root = next(m for m in ET.parse(path).getroot().findall(ns + "menu")
                if m.get("id") == "root-menu")

    def walk(m):
        out = []
        for c in m:
            if not isinstance(c.tag, str):
                continue
            tag = c.tag[len(ns):]
            cmd = None
            for a in c.findall(ns + "action"):
                for o in a:
                    cmd = o.text
            kids = walk(c) if tag == "menu" else []
            out.append((tag, dict(c.attrib), kids, cmd))
        return out
    return walk(root)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-menu-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    menu_xml = os.path.join(cfg, "waybox", "menu.xml")
    with open(menu_xml, "w") as f:
        f.write(MENU)
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"></openbox_config>\n')
    # An installed program, in the user's applications.
    apps = os.path.join(workdir, "data", "applications")
    os.makedirs(apps)
    with open(os.path.join(apps, "test-editor.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Test Editor\n"
                "Exec=test-editor %F\nIcon=accessories-text-editor\n")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=os.path.join(workdir, "data"), XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
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
    app = subprocess.Popen([wbconf], env=dict(wenv, WBCONF_TEST_TOC=STEPS),
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * 29 + 2)
    app.kill()
    lines = app.communicate()[1].splitlines()

    def dumps():
        out = []
        for l in lines:
            if l.startswith("menu-dump:"):
                out.append((l, []))
            elif l.startswith("menu|") and out:
                out[-1][1].append(l)
        return out
    d = dumps()
    failures = []
    if not d or d[0][1] != WANT_LOADED or d[0][0] != "menu-dump:0":
        failures.append("as loaded:\n  " + "\n  ".join(d[0][1] if d else []))
    picked = next((l[len("menu-iconpick:"):] for l in lines if l.startswith("menu-iconpick:")), "")
    icon8 = picked or "accessories-text-editor"
    want_edited = [
        "menu|1|Console|utilities-terminal|Run alacritty",
        "menu|2|||A divider",
        "menu|3|Tools||A submenu of 1 entry",
        "menu|3.1|Calculator||Run gnome-calculator",
        "menu|4|Editor||Run gedit",
        "menu|5|||Shows the menu \u201cPlaces\u201d",
        "menu|6|Exit||Log out",
        "menu|7|Games||A submenu of 0 entries",
        f"menu|8|Test Editor|{icon8}|Run test-editor",
    ]
    if len(d) < 2 or d[1][1] != want_edited:
        failures.append("edited:\n  " + "\n  ".join(d[1][1] if len(d) > 1 else []))
    ch = [l for l in lines if l.startswith("changes-dump:")]
    if not ch or ch[0].startswith("changes-dump:0|"):
        failures.append(f"no unsaved changes: {ch[:1]}")
    if len(d) < 3 or d[2][0] != "menu-dump:0" or d[2][1] != want_edited:
        failures.append("after saving:\n  " + (d[2][0] if len(d) > 2 else "") + "\n  " +
                        "\n  ".join(d[2][1] if len(d) > 2 else []))
    text = open(menu_xml).read()
    if "<!-- mine -->" not in text:
        failures.append("the comment was lost")
    got = entries(menu_xml)
    want = [
        ("item", {"label": "Console", "icon": "utilities-terminal"}, [], "alacritty"),
        ("separator", {}, [], None),
        ("menu", {"id": "tools", "label": "Tools"},
         [("item", {"label": "Calculator"}, [], "gnome-calculator")], None),
        ("item", {"label": "Editor"}, [], "gedit"),
        ("menu", {"id": "places"}, [], None),
        ("item", {"label": "Exit"}, [], None),
        # (Its id as made, from the label it had then: ids stay put.)
        ("menu", {"id": "new-submenu", "label": "Games"}, [], None),
        ("item", {"label": "Test Editor", "icon": icon8}, [], "test-editor"),
    ]
    if got != want:
        failures.append(f"menu.xml: {got}")
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
    print("integration wbconf menu: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
