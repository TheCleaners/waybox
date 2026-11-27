#!/usr/bin/env python3
"""Integration test: the Shortcuts page says which bindings never run.

rc.xml has a key bound twice (W-d, then W-D), a single key before a chain
that starts with it (C-a, then C-a x), and a key init.lua binds (W-x). Each
later one is shown as never running, saying why. Then fixed: the dead
W-D removed ("Remove This One"); C-a moved, so the chain runs; a new
binding on W-D takes it from W-d ("Replace It"). Saved: rc.xml has exactly
that. Last, Reset to Defaults: the list becomes Waybox's defaults, as
changes not saved; closing without saving leaves rc.xml alone.
Skips (exit 77) without a headless display.

Usage: wbconf_shortcut_clash_test.py <waybox> <wbconf> <schema-dir> <default-rc>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import wbtest

SKIP = 77
NS = "{http://openbox.org/3.4/rc}"

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-d"><action name="ToggleShowDesktop"/></keybind>
    <keybind key="W-D"><action name="Close"/></keybind>
    <keybind key="C-a"><action name="Iconify"/></keybind>
    <keybind key="C-a"><keybind key="x"><action name="Close"/></keybind></keybind>
    <keybind key="W-x"><action name="Execute"><command>foot</command></action></keybind>
    <keybind key="W-F2"><action name="GoToDesktop"><to>2</to></action></keybind>
    <keybind key="W-z"/>
  </keyboard>
  <mouse>
    <context name="Close">
      <mousebind button="Left" action="Click"><action name="Close"/></mousebind>
    </context>
  </mouse>
</openbox_config>
"""

LUA = """-- wb.bind("W-q", nothing): a comment
wb.bind("W-x", function() wb.action("Close") end)
"""

STEPS = ";".join([
    "click:Shortcuts", "shortcuts", "changes",
    "shortcut-fix:W-D",
    "shortcut-keys:C-a=C-b",
    "shortcut-add:W-D=Iconify",
    "shortcut-order",
    "shortcuts",
    # "2" a whole word of what W-2 does; @problems the rows that never run.
    "search:desktop 2", "shortcut-visible", "search:@problems", "shortcut-visible", "search:",
    "shortcut-fix:W-D",
    "shortcuts",
    # Search: "super d" finds Super+D only (not every row with a "d").
    "search:super d", "shortcut-visible", "search:",
    "save", "status"])


def keybinds(path):
    out = []

    def walk(node, chain):
        for kb in node.findall(NS + "keybind"):
            keys = chain + [kb.get("key")]
            acts = [a.get("name") for a in kb.findall(NS + "action")]
            if acts:
                out.append((" ".join(keys), acts))
            walk(kb, keys)
    walk(ET.parse(path).getroot().find(NS + "keyboard"), [])
    return sorted(out)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, default_rc = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-clash-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc, "w") as f:
        f.write(RC)
    with open(os.path.join(cfg, "waybox", "init.lua"), "w") as f:
        f.write(LUA)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    # A copy for Waybox, without the script: it isn't what's tested.
    wbcfg = os.path.join(workdir, "wbconfig")
    os.makedirs(os.path.join(wbcfg, "waybox"))
    with open(os.path.join(wbcfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=wbcfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
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
                XDG_CONFIG_HOME=cfg, DBUS_SESSION_BUS_ADDRESS="disabled:",
                GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WBCONF_DEFAULT_RC=default_rc,
                WAYBOX_PID=str(proc.pid))

    def run(steps, wait):
        app = subprocess.Popen([wbconf], env=dict(wenv, WBCONF_TEST_TOC=steps),
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        time.sleep(wait)
        app.kill()
        return app.communicate()[1].splitlines()

    def dumps(lines):
        """Each "shortcuts" step's key rows, in turn."""
        out = []
        for l in lines:
            if l.startswith("shortcut-dump:"):
                out.append([])
            elif l.startswith("shortcut:key|") and out:
                out[-1].append(l[len("shortcut:key|"):])
        return out

    failures = []
    lines = run(STEPS, 0.8 * 22 + 2)
    d = dumps(lines)
    want_first = [
        "W-d|Show the desktop|Desktops",
        "W-D|Close the window|Windows|never:W-d",
        "C-a|Minimize|Windows",
        "C-a x|Close the window|Windows|never:C-a",
        "W-x|Run foot|Programs and menus|never:script",
        "W-F2|Go to desktop 2|Desktops",
        "W-z||Other",
    ]
    if not d or d[0] != want_first:
        failures.append("as loaded:\n  " + "\n  ".join(d[0] if d else []))
    # The new W-D comes after W-d once saved: it's the one that never runs,
    # and can take the keys.
    want_mid = [
        "W-d|Show the desktop|Desktops",
        "W-D|Close the window|Windows|removed",
        "C-b|Minimize|Windows|changed",
        "C-a x|Close the window|Windows",
        "W-x|Run foot|Programs and menus|never:script",
        "W-F2|Go to desktop 2|Desktops",
        "W-z||Other",
        "W-D|Minimize|Windows|changed|never:W-d",
    ]
    if len(d) < 2 or d[1] != want_mid:
        failures.append("fixing:\n  " + "\n  ".join(d[1] if len(d) > 1 else []))
    want_last = [
        "W-d|Show the desktop|Desktops|removed",
        "W-D|Close the window|Windows|removed",
        "C-b|Minimize|Windows|changed",
        "C-a x|Close the window|Windows",
        "W-x|Run foot|Programs and menus|never:script",
        "W-F2|Go to desktop 2|Desktops",
        "W-z||Other",
        "W-D|Minimize|Windows|changed",
    ]
    if len(d) < 3 or d[2] != want_last:
        failures.append("replaced:\n  " + "\n  ".join(d[2] if len(d) > 2 else []))
    vis = [l for l in lines if l.startswith("shortcut-visible:")]
    # "desktop 2": W-F2 by what it does (and the new row, being set up);
    # @problems: the script's key, and the click on a place Waybox doesn't
    # read; "super d": Super+D only.
    want_vis = ["shortcut-visible:W-F2,W-D|suggest:",
                "shortcut-visible:W-x,Close Left Click|suggest:",
                "shortcut-visible:W-d,W-D,W-D|suggest:"]
    if vis != want_vis:
        failures.append(f"searching: {vis}")
    # A binding with no actions isn't a change just by being shown.
    ch = [l for l in lines if l.startswith("changes-dump:")]
    if not ch or not ch[0].startswith("changes-dump:0|"):
        failures.append(f"changes as loaded: {ch[:1]}")
    # A new row: at the end of the list, above its foot.
    order = [l for l in lines if l.startswith("shortcut-order:")]
    items = order[0][len("shortcut-order:"):].split(",") if order else []
    if len(items) < 2 or items[-1] != "foot" or items[-2] != "W-D" or "W-D" not in items[:-2]:
        failures.append(f"new row placed: {order}")
    mouse = [l for l in lines if l.startswith("shortcut:mouse|")]
    if not mouse or mouse[0] != "shortcut:mouse|Close Left Click|Close the window|Windows|never:place":
        failures.append(f"a place Waybox doesn't read: {mouse[:1]}")
    got = keybinds(rc)
    want_rc = sorted([("C-a x", ["Close"]), ("W-x", ["Execute"]), ("C-b", ["Iconify"]),
                      ("W-D", ["Iconify"]), ("W-F2", ["GoToDesktop"])])
    if got != want_rc:
        failures.append(f"rc.xml: {got}")

    # Reset to Defaults, not saved.
    before = open(rc).read()
    lines = run("click:Shortcuts;shortcut-reset:keys;shortcuts;changes", 0.8 * 5 + 2)
    d = dumps(lines)
    defaults = keybinds(default_rc)
    shown = [r for r in (d[0] if d else []) if "|removed" not in r]
    if len(shown) != len(defaults):
        failures.append(f"after reset: {len(shown)} bindings, not the defaults' {len(defaults)}")
    if not all(r.endswith(("|changed", "|removed")) or "|changed|" in r or "|never:" in r
               or r.split("|")[0] in [k for k, _ in defaults] for r in (d[0] if d else [])):
        failures.append("after reset, rows not marked")
    ch = [l for l in lines if l.startswith("changes-dump:")]
    if not ch or ch[0].startswith("changes-dump:0|"):
        failures.append(f"reset left no unsaved changes: {ch}")
    if open(rc).read() != before:
        failures.append("reset wrote rc.xml without saving")

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
    print("integration wbconf shortcut clashes: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
