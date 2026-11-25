#!/usr/bin/env python3
"""Integration test: the applications menu (menu.xml <menu type="applications">).

1. Installed programs (test .desktop files in $XDG_DATA_HOME) show by
   category; choosing one runs it, with its field codes removed.
2. A Terminal=true program runs in $TERMINAL -e.
3. Programs hidden by NoDisplay, OnlyShowIn or a missing TryExec don't show:
   the category holds only the two that do.
4. A program installed while Waybox runs shows the next time the menu
   opens.
Skips (77) without wtype or a headless display.

Usage: apps_menu_test.py <waybox-binary>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-r"><action name="ShowMenu"><menu>root-menu</menu></action></keybind>
  </keyboard>
</openbox_config>
"""

MENU_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
<menu id="root-menu" label="Waybox">
  <menu id="applications" label="Applications" type="applications"/>
</menu>
</openbox_menu>
"""


def entry(name, exec_, categories, extra=""):
    return (f"[Desktop Entry]\nType=Application\nName={name}\nExec={exec_}\n"
            f"Categories={categories}\n{extra}")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-apps-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    data = os.path.join(workdir, "data")
    apps = os.path.join(data, "applications")
    empty = os.path.join(workdir, "nodata")
    for d in (os.path.join(cfg, "waybox"), apps, empty):
        os.makedirs(d)
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC_XML)
    with open(os.path.join(cfg, "waybox", "menu.xml"), "w") as f:
        f.write(MENU_XML)
    ran = os.path.join(workdir, "ran")
    term = os.path.join(workdir, "term")
    fake_term = os.path.join(workdir, "fake-term")
    with open(fake_term, "w") as f:
        f.write(f'#!/bin/sh\nprintf "%s\\n" "$*" > {term}\n')
    os.chmod(fake_term, 0o755)
    files = {
        # Accessories: Alpha, then Beta (by name); the hidden ones aren't.
        "beta.desktop": entry("Beta", f"sh -c 'echo beta > {ran}' %U", "Utility;"),
        "alpha.desktop": entry("Alpha", f"sh -c 'echo alpha > {ran}' %f", "GTK;Utility;"),
        "nodisplay.desktop": entry("Hidden1", "true", "Utility;", "NoDisplay=true\n"),
        "gnome.desktop": entry("Hidden2", "true", "Utility;", "OnlyShowIn=GNOME;\n"),
        "tryexec.desktop": entry("Hidden3", "true", "Utility;", "TryExec=/nonexistent/x\n"),
        # Development: one Terminal=true program.
        "tui.desktop": entry("Tui", "htop --tree", "Development;", "Terminal=true\n"),
    }
    for name, text in files.items():
        with open(os.path.join(apps, name), "w") as f:
            f.write(text)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=data, XDG_DATA_DIRS=empty,
               XDG_RUNTIME_DIR=runtime, XDG_CURRENT_DESKTOP="Waybox:wlroots",
               TERMINAL=fake_term, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def keys(*names):
        for name in names:
            args = ["-M", "logo", "-k", name[2:], "-m", "logo"] if name.startswith("W-") else ["-k", name]
            subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
            time.sleep(0.3)

    def wait_file(path, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if os.path.exists(path) and open(path).read().strip():
                return open(path).read().strip()
            time.sleep(0.1)
        return None

    try:
        # Root menu -> Applications -> Accessories (first) -> Beta (second).
        # (Opening a submenu from the keyboard selects its first item.)
        keys("W-r", "Down", "Right", "Right", "Down", "Return")
        got = wait_file(ran)
        if got != "beta":
            failures.append(f"choosing Beta didn't run it (ran: {got!r})")
        log.flush()
        if "wb-menu open applications/Utility" not in open(log_path).read():
            failures.append("the Accessories category didn't open")
        # Down past the last shown entry wraps to Alpha: only two are shown.
        if os.path.exists(ran):
            os.remove(ran)
        keys("W-r", "Down", "Right", "Right", "Down", "Down", "Return")
        got = wait_file(ran)
        if got != "alpha":
            failures.append(f"hidden programs showed in the menu (ran: {got!r})")
        # Development (second category) -> Tui, in the terminal.
        keys("W-r", "Down", "Right", "Down", "Right", "Return")
        got = wait_file(term)
        if got != "-e htop --tree":
            failures.append(f"a Terminal=true program didn't run in $TERMINAL: {got!r}")
        # Installed meanwhile: a new category shows next time.
        time.sleep(1.1)  # the directory's mtime must move
        with open(os.path.join(apps, "game.desktop"), "w") as f:
            f.write(entry("Game", f"sh -c 'echo game > {ran}'", "Game;"))
        if os.path.exists(ran):
            os.remove(ran)
        # Accessories, Development, Games: the third.
        keys("W-r", "Down", "Right", "Down", "Down", "Right", "Return")
        got = wait_file(ran)
        if got != "game":
            failures.append(f"a program installed meanwhile didn't show (ran: {got!r})")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("waybox did not exit on SIGTERM")
        log.close()
    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration apps menu: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
