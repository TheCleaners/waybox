#!/usr/bin/env python3
"""Integration test: a window asking for attention (xdg-activation) is
marked urgent, and so is its desktop (ext-workspace, as Waybar shows it).

Two desktops; a window "far" on desktop 1, then desktop 2 shown.
1. far asks to be activated with no input behind it (a link handed to an
   open browser): the desktop stays 2; far is urgent (get_windows), and
   desktop 1 is urgent to the workspace client.
2. far focused (desktop 1 shown): no longer urgent, nor its desktop.
3. On the shown desktop, another window "near" asks while far has focus:
   urgent, not focused (a program can't take the keyboard on its own).
4. near on every desktop, desktop 2 shown: desktop 2 is the urgent one.
Skips (exit 77) without a headless display.

Usage: urgency_test.py <waybox> <activate_client> <workspace_client>
"""
import json
import re
import os
import queue
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
    waybox, activate, wsclient = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-urgency-", dir="/tmp")
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

    def client(app_id):
        p = subprocess.Popen([activate, app_id], env=cenv, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        children.append(p)
        line = p.stdout.readline().strip()
        if line != "configured":
            raise RuntimeError(f"{app_id}: {line!r}")
        time.sleep(0.4)
        return p

    def ask(p):
        p.stdin.write("activate\n")
        p.stdin.flush()
        if p.stdout.readline().strip() != "asked":
            raise RuntimeError("the client didn't ask")
        time.sleep(0.4)

    # The workspace client: its last line says each desktop's state.
    ws_lines = queue.Queue()
    ws = subprocess.Popen([wsclient], env=cenv, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True)
    children.append(ws)
    threading.Thread(target=lambda: [ws_lines.put(l.strip()) for l in ws.stdout],
                     daemon=True).start()

    def ws_state():
        last = None
        time.sleep(0.3)
        while True:
            try:
                last = ws_lines.get_nowait()
            except queue.Empty:
                return last

    try:
        far = client("far")
        ipc({"request": "action", "action": "GoToDesktop", "to": "2"})
        time.sleep(0.4)
        # 1. Asked from another desktop: urgent, the desktop not switched.
        ask(far)
        if current_desktop() != 2:
            failures.append(f"switched to desktop {current_desktop()} for a program's request")
        if not window("far").get("urgent"):
            failures.append(f"far not urgent: {window('far')}")
        st = ws_state() or ""
        # " ID:NAME:active|-[:urgent]" each (names have spaces)
        urgent = [m.group(1) for m in re.finditer(r" (\d+):[^:]*:(?:active|-)(:urgent)?", st)
                  if m.group(2)]
        if urgent != ["1"]:
            failures.append(f"only desktop 1 should be urgent to the panel: {st!r}")
        # 2. Focused: no longer urgent.
        ipc({"request": "action", "action": "Focus", "window": "app_id:far"})
        time.sleep(0.4)
        if window("far").get("urgent"):
            failures.append("far still urgent once focused")
        st = ws_state() or ""
        if "urgent" in st:
            failures.append(f"a desktop still urgent: {st!r}")
        # 3. On the shown desktop, a program's own request doesn't take focus.
        near = client("near")
        ipc({"request": "action", "action": "Focus", "window": "app_id:far"})
        time.sleep(0.3)
        ask(near)
        if not window("far").get("focused") or not window("near").get("urgent"):
            failures.append(f"near took focus or isn't urgent: far={window('far')} "
                            f"near={window('near')}")
        # 4. On every desktop and urgent: the desktop shown is the urgent
        # one to the panel, not the one it was stored on.
        ipc({"request": "action", "action": "ToggleOmnipresent", "window": "app_id:near"})
        ipc({"request": "action", "action": "GoToDesktop", "to": "2"})
        time.sleep(0.4)
        keeper = client("keeper")  # has the keyboard on desktop 2
        time.sleep(0.3)
        ask(near)
        if not window("near").get("urgent"):
            failures.append(f"near not urgent on every desktop: {window('near')}")
        st = ws_state() or ""
        urgent = [m.group(1) for m in re.finditer(r" (\d+):[^:]*:(?:active|-)(:urgent)?", st)
                  if m.group(2)]
        if urgent != ["2"]:
            failures.append(f"an urgent window on every desktop: the shown one, 2, urgent: {st!r}")
    except (OSError, RuntimeError, ValueError) as e:
        failures.append(str(e))
    finally:
        for c in children:
            c.kill()
            c.wait()
        time.sleep(0.3)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration urgency: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
