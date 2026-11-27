#!/usr/bin/env python3
"""Integration test: the About card (ShowAbout).

1. ShowAbout opens it (the log says so, with the version).
2. A key closes it, and goes no further: Super+F2 (bound to GoToDesktop 2)
   leaves desktop 1 shown.
3. Opened again, a click closes it.
Skips (exit 77) without wtype or wlrctl, or a headless display.

Usage: about_card_test.py <waybox>
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="W-F2"><action name="GoToDesktop"><to>2</to></action></keybind>
  </keyboard>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-about-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def ipc(request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
            c.settimeout(5)
            c.connect(os.path.join(runtime, f"waybox-{sock}.sock"))
            c.sendall((json.dumps(request) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = c.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf)

    def logtext():
        return open(log_path).read()

    def count(s):
        return logtext().count(s)

    def current():
        for d in ipc({"request": "get_desktops"}).get("desktops", []):
            if d.get("current"):
                return d["number"]
        return None

    try:
        # 1. Opened.
        ipc({"request": "action", "action": "ShowAbout"})
        time.sleep(0.4)
        if count("wb-about open") != 1:
            failures.append("ShowAbout didn't open the card")
        # 2. A key closes it, and its binding doesn't run.
        subprocess.run(["wtype", "-M", "logo", "-k", "F2", "-m", "logo"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(0.4)
        if count("wb-about close") != 1:
            failures.append("a key didn't close the card")
        if current() != 1:
            failures.append(f"the key that closed the card also ran its binding: "
                            f"desktop {current()}")
        # 3. A click closes it.
        ipc({"request": "action", "action": "ShowAbout"})
        time.sleep(0.4)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(0.4)
        if count("wb-about close") != 2:
            failures.append("a click didn't close the card")
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in logtext() for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration about card: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
