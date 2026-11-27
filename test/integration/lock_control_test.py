#!/usr/bin/env python3
"""Integration test: while the screen is locked, other clients can't change
the session behind it.

Two desktops; an X11 window "xone" (if Xwayland is there), then foot "one"
and foot "two" (focused) on desktop 1. A workspace client (a panel) queues
an activation of desktop 2, not yet committed. Then the screen is locked:

1. The queued activation committed while locked, and one made and
   committed while locked: the desktop stays 1.
2. wlr-foreign-toplevel (a taskbar): focus "one", close "one", minimize,
   maximize and fullscreen "two": nothing changes.
3. xone asks the window manager to activate it (_NET_ACTIVE_WINDOW): it's
   marked urgent, not focused.

After unlock: "two" has the focus again (nothing changed the focus order
behind the lock), the desktop is 1, and a commit then doesn't apply what
was queued before or during the lock.

Skips (exit 77) without foot, wlrctl or a headless display.

Usage: lock_control_test.py <waybox> <lock_client> <workspace_client>
"""
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
</openbox_config>
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, lock_bin, wsclient = sys.argv[1:]
    for tool in ("foot", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-lockctl-", dir="/tmp")
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
    children = []

    def logtext():
        log.flush()
        return open(log_path).read()

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

    def window(app_id):
        for w in ipc({"request": "get_windows"}).get("windows", []):
            if w["app_id"] == app_id:
                return w
        return {}

    def current_desktop():
        for d in ipc({"request": "get_desktops"}).get("desktops", []):
            if d.get("current"):
                return d["number"]
        return None

    def settle(check, timeout=8.0):
        end = time.time() + timeout
        while not check():
            if time.time() >= end:
                return False
            time.sleep(0.1)
        return True

    def wlrctl(*args):
        subprocess.run(["wlrctl", "toplevel", *args], env=cenv, capture_output=True,
                       timeout=10)

    try:
        # An X11 window first, if there's Xwayland (and python-xlib).
        x11 = None
        m = re.search(r"Xwayland on DISPLAY=(:\d+)", logtext())
        if m is not None:
            try:
                import Xlib  # noqa: F401
                x11 = subprocess.Popen(
                    [sys.executable, os.path.join(os.path.dirname(__file__), "x11_client.py")],
                    env=dict(env, DISPLAY=m.group(1)), stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, text=True)
                children.append(x11)
                x11.stdin.write("window xone 200 150 336699\n")
                x11.stdin.flush()
                if x11.stdout.readline().strip() != "mapped xone" or \
                        not settle(lambda: window("xone").get("focused")):
                    failures.append("the X11 window didn't map with focus")
            except ImportError:
                x11 = None
        for name in ("one", "two"):
            children.append(subprocess.Popen(["foot", "-a", name, "sleep", "300"], env=cenv,
                                             stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL))
            if not settle(lambda: window(name).get("focused")):
                failures.append(f"{name} didn't open with focus")

        ws_lines = queue.Queue()
        ws = subprocess.Popen([wsclient], env=cenv, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
        children.append(ws)
        threading.Thread(target=lambda: [ws_lines.put(l.strip()) for l in ws.stdout],
                         daemon=True).start()

        def panel(cmd):
            ws.stdin.write(cmd + "\n")
            ws.stdin.flush()
            time.sleep(0.4)
        time.sleep(0.5)
        panel("queue 2")  # before the lock, committed during it

        locker = subprocess.Popen([lock_bin], env=cenv, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        children.append(locker)
        for _ in range(30):
            if "locked" in (locker.stdout.readline() or ""):
                break
        else:
            failures.append("the screen didn't lock")

        # 1. Desktops.
        panel("commit")
        panel("activate 2")
        if current_desktop() != 1:
            failures.append(f"a panel switched the desktop while locked: {current_desktop()}")

        # 2. A taskbar.
        wlrctl("focus", "app_id:one")
        wlrctl("close", "app_id:one")
        wlrctl("minimize", "app_id:two")
        wlrctl("maximize", "app_id:two")
        wlrctl("fullscreen", "app_id:two")
        time.sleep(0.6)
        if not window("one"):
            failures.append("a taskbar closed a window while locked")
        two = window("two")
        if two.get("minimized") or two.get("maximized") or two.get("fullscreen"):
            failures.append(f"a taskbar changed a window while locked: {two}")

        # 3. X11 activation: marked, not focused.
        if x11 is not None:
            x11.stdin.write("activate xone\n")
            x11.stdin.flush()
            if not settle(lambda: window("xone").get("urgent"), 4.0):
                failures.append("an X11 window asking to be activated while locked "
                                "wasn't marked urgent")

        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        if not settle(lambda: window("two").get("focused"), 4.0):
            focused = [w["app_id"] for w in ipc({"request": "get_windows"})["windows"]
                       if w.get("focused")]
            failures.append(f"after unlock, the focus isn't back on two: {focused}")
        panel("commit")
        if current_desktop() != 1:
            failures.append("a switch queued before or during the lock applied after it")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for c in children:
            if c.poll() is None:
                c.kill()
            c.wait()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    text = open(log_path).read()
    if any(s in text for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(text[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lock_control: OK" + ("" if x11 else " (no X11 part)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
