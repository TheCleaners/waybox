#!/usr/bin/env python3
"""Integration test: drag-and-drop between two windows.

A press in the source window starts a drag (text/plain, with an icon);
the pointer carries it over the target window and lets go there. Waybox
must start the drag (wlroots only asks), show the icon on screen, and the
target must get "enter" and then the data on drop.

Then a tear-off, as a browser tab is torn off into a window of its own
(xdg-toplevel-drag): the drag carries a new window, which must open at
the pointer (at the client's offset), follow it, and not get in the way
of the drop target under it.

Usage: dnd_test.py <waybox> <dnd_client> <vpointer_client>
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import json
import socket
import threading
import time
import wbtest

SKIP = 77


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
    waybox, dnd_bin, vptr_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-dnd-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><virtualInput allow="always"/><ipc enabled="yes"/></waybox>
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
    clients = []
    try:
        target = Client([dnd_bin, "target"], cenv)
        clients.append(target)
        if not target.wait_for("configured"):
            raise RuntimeError("the target window never showed")
        source = Client([dnd_bin, "source"], cenv)
        clients.append(source)
        if not source.wait_for("configured"):
            raise RuntimeError("the source window never showed")
        time.sleep(0.5)
        log.flush()
        text = open(log_path).read()
        geo = {}
        for app in ("dnd-source", "dnd-target"):
            m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) " + app, text)
            if not m:
                raise RuntimeError(f"no geometry for {app}")
            geo[app] = tuple(map(int, m[-1]))
        sw, sh, sx, sy = geo["dnd-source"]
        tw, th, tx, ty = geo["dnd-target"]
        # A point of the target the source (on top) doesn't cover.
        spot = None
        for px in range(tx + 5, tx + tw - 5, 10):
            for py in range(ty + 5, ty + th - 5, 10):
                if not (sx <= px < sx + sw and sy <= py < sy + sh):
                    spot = (px, py)
                    break
            if spot:
                break
        if spot is None:
            raise RuntimeError(f"the source covers the target: {geo}")
        ptr = Client([vptr_bin], cenv, stdin=True)
        clients.append(ptr)
        ptr.send("move -5000 -5000")
        cx, cy = sx + sw // 2, sy + sh // 2
        ptr.send(f"move {cx} {cy}")
        ptr.send("press")
        if not source.wait_for("drag-started"):
            raise RuntimeError(f"the source never started a drag: {source.drain(0.3)} {target.drain(0.1)}")
        # Carry it over in steps.
        steps = 8
        dx, dy = spot[0] - cx, spot[1] - cy
        for i in range(steps):
            ptr.send(f"move {dx // steps} {dy // steps}")
        ptr.send(f"move {dx - steps * (dx // steps)} {dy - steps * (dy // steps)}")
        if not target.wait_for("enter", timeout=2.0):
            failures.append("the drag never reached the target window")
        ptr.send("release")
        if not target.wait_for("dropped hello-dnd", timeout=3.0):
            failures.append(f"the target didn't get the data: {target.drain(0.2)}")
        seen = source.drain(0.5)
        if "icon-on-output" not in seen:
            failures.append(f"the drag icon wasn't shown: {seen}")
        if "drop-performed" not in seen or "finished" not in seen:
            failures.append(f"the source didn't see the drop finish: {seen}")

        # ---- the tear-off ----
        source.proc.kill()
        source.proc.wait()
        tear = Client([dnd_bin, "tearoff"], cenv)
        clients.append(tear)
        if not tear.wait_for("configured"):
            raise RuntimeError("the tear-off window never showed")
        time.sleep(0.5)
        log.flush()
        m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) dnd-source",
                       open(log_path).read())
        ow, oh, ox, oy = map(int, m[-1])
        # The pointer is at the spot over the target; into the new window.
        px, py = spot
        nx, ny = ox + ow // 2, oy + oh // 2
        ptr.send(f"move {nx - px} {ny - py}")
        px, py = nx, ny
        ptr.send("press")
        if not tear.wait_for("torn-mapped", timeout=3.0):
            raise RuntimeError(f"the torn-off window never showed: {tear.drain(0.2)}")
        time.sleep(0.3)
        log.flush()
        text = open(log_path).read()
        m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) dnd-torn", text)
        if not m:
            failures.append("no geometry for the torn-off window")
        elif (int(m[-1][2]), int(m[-1][3])) != (px - 20, py - 10):
            failures.append(f"the torn-off window didn't open at the pointer: "
                            f"{m[-1][2:]} vs {(px - 20, py - 10)}")
        # Carry it over the target.
        tx2, ty2 = tx + tw // 2, ty + th // 2
        for i in range(6):
            ptr.send(f"move {(tx2 - px) // 6} {(ty2 - py) // 6}")
        ptr.send(f"move {(tx2 - px) - 6 * ((tx2 - px) // 6)} {(ty2 - py) - 6 * ((ty2 - py) // 6)}")
        px, py = tx2, ty2
        time.sleep(0.2)
        torn = [w for w in ipc(runtime, sock, {"request": "get_windows"}).get("windows", [])
                if w.get("app_id") == "dnd-torn"]
        if not torn or (torn[0]["x"], torn[0]["y"]) != (px - 20, py - 10):
            failures.append(f"the torn-off window didn't follow the pointer: "
                            f"{[(w['x'], w['y']) for w in torn]} vs {(px - 20, py - 10)}")
        if not target.wait_for("enter", timeout=2.0):
            failures.append("with a window carried, the drag never reached the target under it")
        ptr.send("release")
        if not target.wait_for("dropped hello-dnd", timeout=3.0):
            failures.append(f"the target didn't get the tear-off's drop: {target.drain(0.2)}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for c in clients:
            c.proc.kill()
            c.proc.wait()
    time.sleep(0.2)
    if proc.poll() is not None:
        failures.append("waybox exited")
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
        print(f"log: {log_path}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration dnd: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
