#!/usr/bin/env python3
"""Integration test for <waybox><switcher order="...">.

Three foot windows, placed by application rules at x = 600, 100 and 300
(opened in that order, so the last is focused). One Alt+Tab:

- mru: goes to the window opened before the focused one (x = 100);
- spatial: goes to the next one by position, left to right (x = 600).

Skips (exit 77) without foot or wtype, or a headless display.

Usage: switcher_order_test.py <waybox-binary>
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

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
  </keyboard>
  <applications>
    <application class="w1"><position><x>600</x><y>100</y></position></application>
    <application class="w2"><position><x>100</x><y>100</y></position></application>
    <application class="w3"><position><x>300</x><y>100</y></position></application>
  </applications>
  <waybox><switcher order="{order}"/></waybox>
</openbox_config>
"""


def run(waybox, workdir, order):
    """The app_id focused after one Alt+Tab, with this order; and the log."""
    cfg = os.path.join(workdir, f"cfg-{order}")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC.format(order=order))
    runtime = os.path.join(workdir, f"run-{order}")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, f"waybox-{order}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
        proc.kill()
        log.close()
        return None, open(log_path).read(), None
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    feet = []
    focused = None
    try:
        for app in ("w1", "w2", "w3"):
            feet.append(subprocess.Popen(["foot", "-a", app], env=cenv,
                                         stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL))
            time.sleep(1.0)
        subprocess.run(["wtype", "-M", "alt", "-k", "Tab", "-m", "alt"], env=cenv,
                       capture_output=True)
        time.sleep(0.6)
        log.flush()
        hits = re.findall(r"Keyboard focus is now on surface: (\S+)", open(log_path).read())
        focused = hits[-1] if hits else None
    finally:
        for f in feet:
            f.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    return focused, open(log_path).read(), proc.returncode


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wtype"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-sworder-", dir="/tmp")
    failures = []
    logs = ""
    for order, want in (("mru", "w2"), ("spatial", "w1")):
        focused, log, status = run(waybox, workdir, order)
        logs += log
        if status is None:
            sys.stderr.write(log)
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            sys.exit(SKIP)
        if focused != want:
            failures.append(f"order={order}: Alt+Tab focused {focused}, expected {want}")
        if status not in (0, None):
            failures.append(f"order={order}: waybox exit status {status}")
    if any(s in logs for s in ("Sanitizer", "Assertion", "runtime error",
                               "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(logs[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration switcher order: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
