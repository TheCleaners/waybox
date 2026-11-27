#!/usr/bin/env python3
"""Integration test: the Desktop mouse context (Openbox's desktop windows).

A stand-in for a desktop-icon program covers the output in the background
layer; it prints the buttons it gets.
1. Desktop bindings as Openbox configs have them: a left-press action (as
   Openbox's Focus, Raise) and the wheel run, and the program still gets
   the click and the scroll; a right-press ShowMenu opens the root menu,
   and then the program doesn't get the press (the menu has the pointer).
2. With only a Root binding (as most configs have): the program gets its
   right-click; the Root binding doesn't run on it.
3. The bottom layer counts as the desktop too; the top layer (a panel)
   doesn't.
4. A Desktop binding also covers the empty desktop (Root), as in Openbox.
5. With swaybg (an empty input region), the empty desktop is still Root.
Bindings run Execute actions that leave marker files, or open the menu
(seen in the log).
Skips (77) without wlrctl, wtype or a headless display.

Usage: desktop_context_test.py <waybox> <desktop_client> <vpointer_client>
"""
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

import wbtest

SKIP = 77
BTN_LEFT, BTN_RIGHT = 272, 273

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse>{contexts}</mouse>
</openbox_config>
"""


MENU_XML = """<?xml version="1.0"?>
<openbox_menu><menu id="root-menu" label="Waybox"><item label="Nothing"/></menu></openbox_menu>
"""


def context(name, button, mark, action="Press"):
    return (f'<context name="{name}"><mousebind button="{button}" action="{action}">'
            f'<action name="Execute"><execute>touch {mark}</execute></action>'
            f'</mousebind></context>')


def menu_context(name, button):
    return (f'<context name="{name}"><mousebind button="{button}" action="Press">'
            f'<action name="ShowMenu"><menu>root-menu</menu></action>'
            f'</mousebind></context>')


class Client:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, text=True, stdout=subprocess.PIPE)
        wbtest.reap_on_exit(self.proc)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def wait_for(self, want, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.lines.get(timeout=max(0.01, end - time.time())) == want:
                    return True
            except queue.Empty:
                break
        return False

    def drain(self, seconds=0.6):
        out, end = [], time.time() + seconds
        while time.time() < end:
            try:
                out.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return out


def session(waybox, workdir, tag, contexts, steps):
    cfg = os.path.join(workdir, f"config-{tag}")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC.format(contexts=contexts))
    with open(os.path.join(cfg, "waybox", "menu.xml"), "w") as f:
        f.write(MENU_XML)
    runtime = os.path.join(workdir, f"run-{tag}")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, f"waybox-{tag}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    problems = []

    def menus():
        log.flush()
        return open(log_path).read().count("wb-menu open root-menu")
    try:
        steps(cenv, problems, menus)
    except (RuntimeError, OSError) as e:
        problems.append(str(e))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    text = open(log_path).read()
    if proc.returncode not in (0, None):
        problems.append(f"waybox exit status {proc.returncode}")
    if any(s in text for s in ("Sanitizer", "Assertion", "runtime error")):
        problems.append("sanitizer/assert output")
    return [f"{tag}: {p}" for p in problems]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, vpointer_bin = sys.argv[1:]
    for tool in ("wlrctl", "wtype"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-desktopctx-", dir="/tmp")
    desk_mark = os.path.join(workdir, "desktop-mark")
    root_mark = os.path.join(workdir, "root-mark")
    scroll_mark = os.path.join(workdir, "scroll-mark")

    def clear():
        for m in (desk_mark, root_mark, scroll_mark):
            if os.path.exists(m):
                os.remove(m)

    def click(cenv, button):
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "300", "300"], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", button], env=cenv, capture_output=True)
        time.sleep(0.5)

    def start(cenv, layer):
        c = Client([client_bin, layer], cenv)
        if not c.wait_for("configured"):
            raise RuntimeError(f"the {layer} client never showed")
        time.sleep(0.3)
        return c

    def close_menu(cenv):
        subprocess.run(["wtype", "-k", "Escape"], env=cenv, capture_output=True)
        time.sleep(0.3)

    def desktop_binding(cenv, problems, menus):
        c = start(cenv, "background")
        # Openbox-style non-menu bindings: they run, the program still gets it.
        clear()
        click(cenv, "left")
        got = c.drain()
        if not os.path.exists(desk_mark):
            problems.append("a Desktop left-press binding didn't run")
        if f"button pressed {BTN_LEFT}" not in got or f"button released {BTN_LEFT}" not in got:
            problems.append(f"a Desktop binding (not a menu) kept the click from the program: {got}")
        # A real wheel notch (wlrctl's scrolling isn't a wheel's).
        subprocess.run([vpointer_bin], input="scroll 1\n", env=cenv, capture_output=True,
                       text=True, timeout=10)
        time.sleep(0.5)
        got = c.drain()
        if not os.path.exists(scroll_mark):
            problems.append("a Desktop wheel binding didn't run")
        if "axis" not in got:
            problems.append(f"a Desktop wheel binding kept the scroll from the program: {got}")
        # A menu binding: the menu opens, and the program doesn't get the press.
        before = menus()
        click(cenv, "right")
        got = c.drain()
        if menus() != before + 1:
            problems.append("a Desktop right-click didn't open the root menu")
        if f"button pressed {BTN_RIGHT}" in got:
            problems.append(f"the program got the press that opened the menu: {got}")
        if os.path.exists(root_mark):
            problems.append("the Root binding ran on the desktop program")
        close_menu(cenv)
        c.proc.kill()
        c.proc.wait()
        time.sleep(0.3)
        # 4. With no desktop program, the Desktop binding covers the empty desktop.
        before = menus()
        click(cenv, "right")
        if menus() != before + 1:
            problems.append("a Desktop binding didn't cover the empty desktop")
        close_menu(cenv)
        # 3. Bottom counts; top doesn't.
        for layer, counts in (("bottom", True), ("top", False)):
            c = start(cenv, layer)
            before = menus()
            click(cenv, "right")
            got = c.drain()
            opened = menus() != before
            if counts and (not opened or f"button pressed {BTN_RIGHT}" in got):
                problems.append(f"the {layer} layer wasn't the desktop: {got}")
            if not counts and (opened or f"button pressed {BTN_RIGHT}" not in got):
                problems.append(f"the {layer} layer was taken as the desktop: {got}")
            close_menu(cenv)
            c.proc.kill()
            c.proc.wait()
            time.sleep(0.3)

    def root_only(cenv, problems, menus):
        c = start(cenv, "background")
        clear()
        click(cenv, "right")
        got = c.drain()
        if os.path.exists(root_mark):
            problems.append("a Root binding ran on the desktop program (it's opt-in)")
        if f"button pressed {BTN_RIGHT}" not in got:
            problems.append(f"the program didn't get its right-click: {got}")
        c.proc.kill()
        c.proc.wait()
        time.sleep(0.3)
        clear()
        click(cenv, "right")
        if not os.path.exists(root_mark):
            problems.append("the Root binding didn't run on the empty desktop")
        # 5. A wallpaper with an empty input region leaves the desktop Root.
        if shutil.which("swaybg"):
            bg = subprocess.Popen(["swaybg", "-c", "#204060"], env=cenv,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wbtest.reap_on_exit(bg)
            time.sleep(1.0)
            clear()
            click(cenv, "right")
            if not os.path.exists(root_mark):
                problems.append("over swaybg, the Root binding didn't run")
            bg.kill()
            bg.wait()

    failures = session(waybox, workdir, "desktop",
                       context("Desktop", "Left", desk_mark)
                       + context("Desktop", "Down", scroll_mark, "Click")
                       + menu_context("Desktop", "Right")
                       + context("Root", "Right", root_mark),
                       desktop_binding)
    failures += session(waybox, workdir, "root", context("Root", "Right", root_mark), root_only)
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration desktop context: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
