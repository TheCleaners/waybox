#!/usr/bin/env python3
"""Integration test: minimized windows stay hidden.

A window is minimized; then a panel appears (windows are re-fitted to the
new usable area): it must stay hidden (it was only moved off screen, and
the re-fit brought it back). Activated again (as a taskbar does), it
shows where it was.

Usage: minimize_test.py <waybox> <touch_client> <layer_client>
"""
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
GREEN = "srgb(48,160,96)"   # touch_client "no"'s colour


class Client:
    def __init__(self, argv, env, stdin=False):
        self.proc = subprocess.Popen(argv, env=env, text=True,
                                     stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                     stdout=subprocess.PIPE)
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def drain(self, seconds=0.3):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.seen.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return self.seen

    def wait_for(self, want, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.seen.append(line)
            if line == want:
                return True
        return False

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        if not self.wait_for("ok"):
            raise RuntimeError(f"the virtual pointer didn't answer {cmd!r}")



def ipc(runtime, display, request):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
        c.settimeout(5)
        c.connect(os.path.join(runtime, f"waybox-{display}.sock"))
        c.sendall((json.dumps(request) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = c.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)




def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, layer_bin = sys.argv[1:]
    for tool in ("grim", "magick", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-minimize-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
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
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    clients = []

    def pixel(x, y):
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        return subprocess.run(["magick", shot, "-format", f"%[pixel:p{{{x},{y}}}]", "info:"],
                              capture_output=True, text=True).stdout.strip()

    def window():
        return [w for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]
                if w.get("app_id") == "touch-no"][0]

    try:
        win = Client([client_bin, "no"], cenv)
        clients.append(win)
        if not win.wait_for("configured"):
            raise RuntimeError("the window never showed")
        time.sleep(0.5)
        w0 = window()
        cx, cy = w0["x"] + w0["width"] // 2, w0["y"] + w0["height"] // 2
        if pixel(cx, cy) != GREEN:
            raise RuntimeError(f"the window isn't drawn: {pixel(cx, cy)}")
        r = ipc(runtime, sock, {"request": "action", "action": "Iconify",
                                "window": "app_id:touch-no"})
        if not r.get("ok"):
            raise RuntimeError(f"Iconify failed: {r}")
        time.sleep(0.4)
        if pixel(cx, cy) == GREEN:
            failures.append("a minimized window is still shown")
        bar = subprocess.Popen([layer_bin], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        clients.append(type("P", (), {"proc": bar})())
        time.sleep(1.2)
        if pixel(cx, cy) == GREEN or not window()["minimized"]:
            failures.append("a minimized window came back when a panel appeared")
        subprocess.run(["wlrctl", "toplevel", "focus", "app_id:touch-no"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(0.5)
        w1 = window()
        if w1["minimized"]:
            failures.append("activating a minimized window didn't bring it back")
        elif pixel(w1["x"] + w1["width"] // 2, w1["y"] + w1["height"] // 2) != GREEN:
            failures.append("the window brought back isn't shown")
    except (RuntimeError, OSError, KeyError, IndexError) as e:
        failures.append(str(e))
    finally:
        for c in clients:
            c.proc.kill()
            c.proc.wait()
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration minimize: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
