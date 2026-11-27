#!/usr/bin/env python3
"""Integration test: closing windows from the window switcher.

Four foot windows (w1..w4, w4 focused). Alt held, Tab: the switcher lists
them, w3 selected. While Alt stays held:

1. Delete: w3 is asked to close; it goes, and its row with it; the
   switcher stays (three rows).
2. Shift+Delete: the newly selected window's program is ended (Kill).
3. A click on a row's close button (x): that window closes.
4. A click on the last row: it's picked (focused), the switcher gone.
5. With that one window left, Alt+Tab still opens the switcher.
6. With <switcher close="no">, Delete closes nothing.

Skips (exit 77) without foot, wtype or wlrctl, or a headless display.

Usage: switcher_close_test.py <waybox>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
  </keyboard>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-swclose-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
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
        log.close()
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    feet = {}

    def text():
        log.flush()
        return open(log_path).read()

    def panels():
        return [tuple(map(int, m)) for m in re.findall(
            r"wb-switcher-panel (\d+),(\d+) (\d+)x(\d+) rows (\d+) row_h (\d+) pad (\d+)", text())]

    def alive(app):
        return feet[app].poll() is None

    def settle(check, timeout=3.0):
        end = time.time() + timeout
        while not check():
            if time.time() >= end:
                return False
            time.sleep(0.1)
        return True

    def rows():
        p = panels()
        return p[-1][4] if p else None

    def click(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv, capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=cenv, capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)

    try:
        for app in ("w1", "w2", "w3", "w4"):
            # (A child that ends at once when foot closes: a login shell can
            # keep foot's window up for seconds.)
            feet[app] = subprocess.Popen(["foot", "-a", app, "sleep", "300"], env=cenv,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.0)
        # Alt held throughout: Tab, Delete, Shift+Delete, then a long pause
        # for the clicks; then let go.
        hold = subprocess.Popen(
            ["wtype", "-M", "alt", "-k", "Tab", "-s", "1200", "-k", "Delete", "-s", "4000",
             "-M", "shift", "-k", "Delete", "-m", "shift", "-s", "9000", "-m", "alt"],
            env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # 1. Delete closed w3; the switcher stays, three rows.
        if not settle(lambda: "wb-switcher close w3" in text(), 4.0):
            failures.append("Delete didn't close the selected window (w3)")
        if not settle(lambda: not alive("w3")):
            failures.append("w3 is still running after its close")
        if not settle(lambda: rows() == 3):
            failures.append(f"the switcher didn't stay with three rows: {rows()}")
        # 2. Shift+Delete: the selected one's program ended.
        if not settle(lambda: "wb-switcher kill " in text(), 5.0):
            failures.append("Shift+Delete didn't end the selected window's program")
        else:
            killed = re.findall(r"wb-switcher kill (\S+)", text())[0]
            if not settle(lambda: not alive(killed)):
                failures.append(f"{killed} still runs after Shift+Delete")
            settle(lambda: rows() == 2)
        # 3. A click on the first row's close button.
        p = panels()
        if not p or p[-1][4] != 2:
            failures.append(f"expected two rows left: {p[-1:] if p else p}")
        else:
            x, y, w, _h, _n, rh, pad = p[-1]
            before = set(re.findall(r"wb-switcher close (\S+)", text()))
            click(x + w - 18, y + pad + rh // 2)
            settle(lambda: set(re.findall(r"wb-switcher close (\S+)", text())) - before)
            closed = set(re.findall(r"wb-switcher close (\S+)", text())) - before
            if len(closed) != 1:
                failures.append(f"the close button didn't close one window: {closed}")
            # 4. A click on the remaining row picks it.
            settle(lambda: rows() == 1)
            p = panels()
            if not p or p[-1][4] != 1:
                failures.append(f"expected one row left: {p[-1:] if p else p}")
            else:
                x, y, w, _h, _n, rh, pad = p[-1]
                click(x + 12, y + pad + rh // 2)
                if not settle(lambda: "wb-cycle-outline hidden" in text()):
                    failures.append("a click on a row didn't pick it (the switcher is still up)")
        hold.wait(timeout=25)
        # 5. One window left: Alt+Tab still opens the switcher (to close it).
        shown = len(panels())
        subprocess.run(["wtype", "-M", "alt", "-k", "Tab", "-s", "800", "-m", "alt"], env=cenv,
                       capture_output=True)
        if not settle(lambda: len(panels()) > shown and rows() == 1):
            failures.append("with one window, Alt+Tab didn't open the switcher")
        # 6. <switcher close="no">: Delete closes nothing.
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(RC.replace("</openbox_config>",
                               '<waybox><switcher close="no"/></waybox></openbox_config>'))
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.8)
        closes = text().count("wb-switcher close")
        subprocess.run(["wtype", "-M", "alt", "-k", "Tab", "-s", "600", "-k", "Delete", "-s", "600",
                        "-m", "alt"], env=cenv, capture_output=True)
        time.sleep(0.6)
        left = [a for a in feet if alive(a)]
        if text().count("wb-switcher close") != closes or len(left) != 1:
            failures.append(f"with close=\"no\", Delete closed a window: {left}")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for f in feet.values():
            if f.poll() is None:
                f.kill()
            f.wait()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    t = open(log_path).read()
    if any(s in t for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(t[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration switcher_close: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
