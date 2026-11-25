#!/usr/bin/env python3
"""Integration test: a button release Waybox takes still reaches the
program that got the press.

A button held in a window, a menu opened by key meanwhile, the button
released (over the menu: Waybox takes it), the menu closed: the program
must still get later clicks of that button. (wlroots counts each press it
passes on; one never released stayed held, so no program got that button
again, and drags and moves from programs were refused.)

Usage: stuck_button_test.py <waybox> <touch_client> <vpointer_client>
"""
import os
import queue
import re
import shutil
import signal
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




def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, vptr_bin = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-stuck-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-m"><action name="ShowMenu"><menu>client-menu</menu></action></keybind>
  </keyboard>
  <waybox><virtualInput allow="always"/></waybox>
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

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
        time.sleep(0.4)

    try:
        win = Client([client_bin, "no"], cenv)
        clients.append(win)
        if not win.wait_for("configured"):
            raise RuntimeError("the window never showed")
        time.sleep(0.5)
        log.flush()
        m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) touch-no",
                       open(log_path).read())
        if not m:
            raise RuntimeError("no geometry for the window")
        w, h, x, y = map(int, m[-1])
        ptr = Client([vptr_bin], cenv, stdin=True)
        clients.append(ptr)
        ptr.send("move -5000 -5000")
        ptr.send(f"move {x + w // 2} {y + h // 2}")
        win.drain(0.2)

        ptr.send("press")
        keys("-M", "logo", "-k", "m", "-m", "logo")   # the window menu opens
        ptr.send("release")                          # Waybox takes this one
        keys("-k", "Escape")                         # the menu closes
        win.drain(0.3)
        win.seen.clear()
        ptr.send("press")
        ptr.send("release")
        seen = win.drain(0.4)
        if seen.count("button pressed") != 1 or seen.count("button released") != 1:
            failures.append(f"after a release Waybox took, the program no longer "
                            f"gets that button: {seen}")
    except RuntimeError as e:
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
    print("integration stuck button: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
