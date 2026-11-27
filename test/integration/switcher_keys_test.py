#!/usr/bin/env python3
"""Integration test: the switcher's keys, set in rc.xml.

<switcher closeKey="x" killKey="none"/>. A foot window "f1", then a
window that ignores being asked to close ("stub", focused). Alt held,
Tab: f1 selected. While Alt stays held:

1. x: f1 is asked to close, and goes; stub is selected.
2. Shift+Delete (the default kill key, off here): nothing.
3. x, and x again: stub is asked to close, twice; it doesn't close, but
   it answers pings (as one asking to save would): its program is NOT
   ended.
4. stub frozen (SIGSTOP): x asks it again and pings it; unanswered in
   2.5 s, its row says it isn't responding; x then ends its program
   (and x again, as Kill's second time, forces it).

Skips (exit 77) without foot or wtype, or a headless display.

Usage: switcher_keys_test.py <waybox> <activate_client>
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
  <waybox><switcher closeKey="x" killKey="none"/></waybox>
</openbox_config>
"""


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, stub_bin = sys.argv[1:]
    for tool in ("foot", "wtype"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-swkeys-", dir="/tmp")
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
    kids = []

    def text():
        log.flush()
        return open(log_path).read()

    def settle(check, timeout=3.0):
        end = time.time() + timeout
        while not check():
            if time.time() >= end:
                return False
            time.sleep(0.1)
        return True

    def rows():
        p = re.findall(r"wb-switcher-panel \S+ \S+ rows (\d+)", text())
        return int(p[-1]) if p else None

    try:
        f1 = subprocess.Popen(["foot", "-a", "f1", "sleep", "300"], env=cenv,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kids.append(f1)
        time.sleep(1.0)
        stub = subprocess.Popen([stub_bin, "stub"], env=cenv, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        kids.append(stub)
        if stub.stdout.readline().strip() != "configured":
            failures.append("the stubborn window didn't show")
        time.sleep(0.5)
        hold = subprocess.Popen(
            ["wtype", "-M", "alt", "-k", "Tab", "-s", "1000", "-k", "x", "-s", "1500",
             "-M", "shift", "-k", "Delete", "-m", "shift", "-s", "1000",
             "-k", "x", "-s", "1500", "-k", "x", "-s", "2000",
             "-k", "x", "-s", "4000", "-k", "x", "-s", "700", "-k", "x", "-s", "1500",
             "-m", "alt"],
            env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kids.append(hold)
        # 1. x closed f1.
        if not settle(lambda: "wb-switcher close f1" in text(), 4.0):
            failures.append("x didn't close the selected window (f1)")
        if not settle(lambda: f1.poll() is not None and rows() == 1):
            failures.append(f"f1 didn't go (rows {rows()})")
        # 2. Shift+Delete: off.
        time.sleep(1.6)
        if "wb-switcher kill" in text():
            failures.append("killKey=\"none\": Shift+Delete still ended a program")
        # 3. x, x: stub asked twice; it answers pings: not ended.
        if not settle(lambda: text().count("wb-switcher close stub") >= 2, 5.0):
            failures.append("x didn't ask the stubborn window to close (twice)")
        time.sleep(0.8)
        if stub.poll() is not None or "wb-switcher kill stub" in text():
            failures.append("a window that answers pings (asking to save?) was ended")
        # 4. Frozen: asked and pinged; unanswered: not responding; then ended.
        os.kill(stub.pid, signal.SIGSTOP)
        if not settle(lambda: text().count("wb-switcher close stub") >= 3, 4.0):
            failures.append("x didn't ask the frozen window to close")
        if not settle(lambda: "wb-not-responding stub" in text(), 4.0):
            failures.append("the frozen window wasn't found not responding")
        if not settle(lambda: "wb-switcher kill stub" in text(), 4.0):
            failures.append("x on a window not responding didn't end its program")
        if not settle(lambda: stub.poll() is not None, 5.0):
            failures.append("the frozen window's program still runs")
        hold.wait(timeout=30)
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for k in kids:
            if k.poll() is None:
                k.kill()
            k.wait()
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
    print("integration switcher_keys: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
