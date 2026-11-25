#!/usr/bin/env python3
"""Integration test: output scale for new windows, and a GPU reset.

1. On an output at scale 2, a new window learns its scale (preferred 240)
   before its first configure, so its first frame is drawn at the right
   scale, and enters its output once.
2. A GPU reset (WAYBOX_TEST_GPU_RESET=1, over IPC): Waybox makes a new
   renderer, and asks windows to draw again (what they showed was on the
   GPU); a window that redraws when configured, as toolkits do, is shown
   again, and a new one opens and is drawn.

Usage: render_reset_test.py <waybox> <scale_client> <touch_client>
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
GREEN = "srgb(48,160,96)"   # touch_client "no"
BLUE = "srgb(48,96,160)"    # touch_client "yes"


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
    waybox, scale_bin, touch_bin = sys.argv[1:]
    for tool in ("grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-render-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <ipc enabled="yes"/>
    <outputs><output name="HEADLESS-1" scale="2"/></outputs>
  </waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               WAYBOX_TEST_GPU_RESET="1", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
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

    def pixel_of(app):
        w = [w for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]
             if w.get("app_id") == app][0]
        x, y = w["x"] + w["width"] // 2, w["y"] + w["height"] // 2
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", "-s", "1", shot], env=cenv, capture_output=True)
        got = subprocess.run(["magick", shot, "-format", f"%[pixel:p{{{x},{y}}}]", "info:"],
                             capture_output=True, text=True).stdout.strip()
        # srgb(r,g,b) or srgba(r,g,b,a): the colour.
        m = re.match(r"srgba?\((\d+),(\d+),(\d+)", got)
        return f"srgb({m.group(1)},{m.group(2)},{m.group(3)})" if m else got

    try:
        # 1. Scale before the first frame.
        sc = Client([scale_bin], cenv)
        clients.append(sc)
        if not sc.wait_for("configured"):
            raise RuntimeError(f"the scale window never showed: {sc.seen}")
        seen = sc.drain(0.5)
        if "preferred 240" not in seen or seen.index("preferred 240") > seen.index("configure"):
            failures.append(f"the scale didn't come before the first configure: {seen}")
        if seen.count("enter") != 1:
            failures.append(f"the window didn't enter its output exactly once: {seen}")

        sc.proc.kill()   # room for two windows side by side (640x360 at scale 2)
        sc.proc.wait()
        time.sleep(0.2)

        # 2. A GPU reset.
        old = Client([touch_bin, "no"], cenv)
        clients.append(old)
        if not old.wait_for("configured"):
            raise RuntimeError("the window never showed")
        time.sleep(0.4)
        if pixel_of("touch-no") != GREEN:
            raise RuntimeError(f"the window isn't drawn: {pixel_of('touch-no')}")
        r = ipc(runtime, sock, {"request": "test_renderer_lost"})
        if not r.get("ok"):
            raise RuntimeError(f"no test GPU reset: {r}")
        time.sleep(0.5)
        log.flush()
        if "wb-renderer re-created" not in open(log_path).read():
            failures.append("the renderer wasn't re-created")
        # Before anything else happens (a new window would change focus,
        # and that configure make it redraw anyway).
        if pixel_of("touch-no") != GREEN:
            failures.append(f"a window from before isn't drawn again after the reset: "
                            f"{pixel_of('touch-no')}")
        new = Client([touch_bin, "yes"], cenv)
        clients.append(new)
        if not new.wait_for("configured"):
            failures.append("no new window after the GPU reset")
        time.sleep(0.5)
        if pixel_of("touch-yes") != BLUE:
            failures.append(f"a new window isn't drawn after the reset: {pixel_of('touch-yes')}")
        if pixel_of("touch-no") != GREEN:
            failures.append(f"a window from before isn't drawn after the reset: "
                            f"{pixel_of('touch-no')}")
    except (RuntimeError, OSError, KeyError, IndexError, ValueError) as e:
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
    print("integration render reset: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
