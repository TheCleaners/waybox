#!/usr/bin/env python3
"""Integration test: touchscreens.

With a fake touchscreen (WAYBOX_TEST_TOUCH=1, driven over IPC):
1. the seat offers touch;
2. a touch on a window that uses touch reaches it as touch (down, motion,
   up, at the right place) and focuses it;
3. on a window without touch support it works as a pointer (left button);
4. on a titlebar it drags the window, as the pointer would.

Usage: touch_test.py <waybox> <touch_client>
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
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-touch-", dir="/tmp")
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
               WAYBOX_TEST_TOUCH="1", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
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

    def geom(app):
        log.flush()
        m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) " + app,
                       open(log_path).read())
        return tuple(map(int, m[-1])) if m else None

    def focused():
        found = re.findall(r"Keyboard focus is now on surface: (\S+)", open(log_path).read())
        return found[-1] if found else None

    try:
        outs = ipc(runtime, sock, {"request": "get_outputs"}).get("outputs", [])
        if not outs:
            raise RuntimeError("no outputs over IPC")
        ow, oh = outs[0]["width"], outs[0]["height"]

        def touch(event, x=0, y=0, tid=0):
            r = ipc(runtime, sock, {"request": "test_touch", "event": event, "id": tid,
                                    "x": x / ow, "y": y / oh})
            if not r.get("ok"):
                raise RuntimeError(f"test_touch: {r}")
            ipc(runtime, sock, {"request": "test_touch", "event": "frame"})
            time.sleep(0.15)

        yes = Client([client_bin, "yes"], cenv)
        clients.append(yes)
        if not yes.wait_for("configured"):
            raise RuntimeError("the touch window never showed")
        no = Client([client_bin, "no"], cenv)
        clients.append(no)
        if not no.wait_for("configured"):
            raise RuntimeError("the pointer window never showed")
        time.sleep(0.5)
        if "has-touch" not in yes.drain(0.1):
            failures.append("the seat doesn't offer touch")
        gy, gn = geom("touch-yes"), geom("touch-no")
        if gy is None or gn is None:
            raise RuntimeError(f"no geometry: {gy} {gn}")

        # 2. Touch on the window that uses it.
        w, h, x, y = gy
        touch("down", x + 150, y + 100)
        touch("motion", x + 170, y + 110)
        touch("up")
        seen = yes.drain(0.3)
        downs = [l for l in seen if l.startswith("touch-down")]
        if not downs or downs[0] != "touch-down 150 100":
            failures.append(f"touch down not at the right place: {seen}")
        if "touch-motion 170 110" not in seen or "touch-up" not in seen:
            failures.append(f"touch motion/up not received: {seen}")
        if focused() != "touch-yes":
            failures.append(f"touching a window didn't focus it ({focused()})")

        # 3. On a window without touch: a pointer.
        w, h, x, y = gn
        touch("down", x + 150, y + 100)
        touch("up")
        seen = no.drain(0.3)
        if "button pressed" not in seen or "button released" not in seen:
            failures.append(f"touch didn't work as a pointer: {seen}")

        # 4. Dragging a titlebar (Waybox's, on foot) moves the window.
        if shutil.which("foot") is None:
            print("note: foot not found, titlebar drag not tested", file=sys.stderr)
        else:
            ft = subprocess.Popen(["foot", "-o", "csd.preferred=server", "--app-id=touchfoot",
                                   "-W", "40x10"], env=cenv, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            clients.append(type("P", (), {"proc": ft})())
            for _ in range(40):
                if geom("touchfoot") is not None:
                    break
                time.sleep(0.1)
            time.sleep(0.5)
            g = geom("touchfoot")
            if g is None:
                raise RuntimeError("foot never showed")
            w, h, x, y = g
            if y < 12:
                raise RuntimeError(f"foot has no room for a titlebar: {g}")

            def pos():
                return [(wi["x"], wi["y"]) for wi in
                        ipc(runtime, sock, {"request": "get_windows"})["windows"]
                        if wi.get("app_id") == "touchfoot"][0]
            before = pos()
            touch("down", x + w // 2, y - 8)
            for i in range(1, 6):
                touch("motion", x + w // 2 + 20 * i, y - 8 + 10 * i)
            touch("up")
            after = pos()
            dx, dy = after[0] - before[0], after[1] - before[1]
            if abs(dx - 100) > 20 or abs(dy - 50) > 20:
                failures.append(f"a titlebar drag by touch didn't move the window: {dx},{dy}")
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
    print("integration touch: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
