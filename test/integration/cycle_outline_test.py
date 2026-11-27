#!/usr/bin/env python3
"""Integration test: the outline around the window the switcher would pick
(Openbox's focus cycle indicator).

Two foot windows, placed by application rules ("w1" at 100,100, then "w2"
at 500,150, focused). Alt held, Tab pressed: the switcher selects w1, and
while Alt is still held:

1. The outline is around w1's whole frame (logged where it was drawn), in
   its colours: the frame's outermost pixel white, two in black.
2. Tab again: it moves to w2.
3. Alt released: the switcher picks, and the outline is gone.

Skips (exit 77) without foot, wtype or grim, or a headless display.

Usage: cycle_outline_test.py <waybox>
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
  <applications>
    <application class="w1"><position><x>100</x><y>100</y></position></application>
    <application class="w2"><position><x>500</x><y>150</y></position></application>
  </applications>
</openbox_config>
"""


def pixel(env, x, y):
    """The (r, g, b) at layout (x, y), from grim (PPM)."""
    r = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"], env=env,
                       capture_output=True, timeout=10)
    data = r.stdout
    # "P6\n1 1\n255\n" then three bytes.
    return tuple(data[-3:]) if r.returncode == 0 and len(data) >= 3 else None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wtype", "grim"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-outline-", dir="/tmp")
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
    children = []

    def text():
        log.flush()
        return open(log_path).read()

    def outlines():
        return re.findall(r"wb-cycle-outline (\S+) (-?\d+),(-?\d+) (\d+)x(\d+)", text())

    try:
        for app in ("w1", "w2"):
            children.append(subprocess.Popen(["foot", "-a", app], env=cenv,
                                             stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL))
            time.sleep(1.2)
        # Alt held through two Tabs, a pause after each; then let go.
        hold = subprocess.Popen(["wtype", "-M", "alt", "-k", "Tab", "-s", "1500",
                                 "-k", "Tab", "-s", "1500", "-m", "alt"],
                                env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        children.append(hold)
        time.sleep(0.9)
        # 1. Around w1's frame, in its colours.
        got = outlines()
        if not got or got[-1][0] != "w1":
            failures.append(f"no outline around the selected window (w1): {got}")
        else:
            _, x, y, w, h = got[-1]
            x, y = int(x), int(y)
            if x > 100 or y > 100 or x < 80 or y < 60:
                failures.append(f"the outline isn't at w1's frame: {got[-1]}")
            outer, band = pixel(cenv, x, y + 20), pixel(cenv, x + 3, y + 20)
            if outer != (255, 255, 255) or band != (0, 0, 0):
                failures.append(f"the outline's colours: outer {outer}, band {band}")
        # 2. The next Tab moves it.
        time.sleep(1.5)
        got = outlines()
        if not got or got[-1][0] != "w2":
            failures.append(f"the outline didn't move to w2: {got[-1:] if got else got}")
        # 3. Released: gone.
        hold.wait(timeout=10)
        time.sleep(0.5)
        if "wb-cycle-outline hidden" not in text():
            failures.append("the outline is still there after the switcher picked")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for c in children:
            if c.poll() is None:
                c.kill()
            c.wait()
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
        sys.stderr.write(t[-2500:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration cycle_outline: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
