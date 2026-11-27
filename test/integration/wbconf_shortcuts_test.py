#!/usr/bin/env python3
"""Integration test: wbconf's Shortcuts page lists rc.xml's bindings.

A chain (C-a then x), an Execute with Openbox's older <execute>, a binding
with If (advanced: left to rc.xml), and mouse bindings in two contexts:
each listed with what it does, under its category.
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
                WAYBOX_PID=str(proc.pid), WBCONF_TEST_TOC="click:Shortcuts;shortcuts")
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * 4 + 1.5)
    app.kill()
    _, err = app.communicate()
    got = [l[len("shortcut:"):] for l in err.splitlines() if l.startswith("shortcut:")]
    failures = []
    if got != WANT:
        failures.append("rows:\n  " + "\n  ".join(got) + "\nnot:\n  " + "\n  ".join(WANT))
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
