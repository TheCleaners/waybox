#!/usr/bin/env python3
"""Integration test for pointer constraints and relative pointer motion.

constraint_client.c is a focused window that can lock or confine the
pointer and prints the pointer and relative-motion events it gets:

1. Locked: moving the mouse sends relative motion and no pointer motion,
   and the cursor stays where it was (after releasing the lock, the next
   motion continues from the old spot, not from where it would have gone).
2. Confined: the pointer cannot be moved out of the window.
3. A lock only holds while the window has keyboard focus: switching
   desktops with the keyboard releases it, and it comes back on return.

Skips (exit 77) without wlrctl, wtype or a headless display.

Usage: pointer_lock_test.py <waybox-binary> <constraint_client-binary>
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
import wbtest

SKIP = 77


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


class Client:
    def __init__(self, binary, env):
        self.proc = subprocess.Popen([binary], env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def send(self, cmd):
        assert self.proc.stdin is not None
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def drain(self, wait=0.4):
        """All lines that arrive within `wait` seconds."""
        out = []
        end = time.time() + wait
        while time.time() < end:
            try:
                out.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return out

    def wait_for(self, prefix, timeout=4.0):
        end = time.time() + timeout
        seen = []
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            seen.append(line)
            if line.startswith(prefix):
                return line, seen
        return None, seen

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1], sys.argv[2]
    for tool in ("wlrctl", "wtype"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-lock-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="W-1"><action name="GoToDesktop"><to>1</to></action></keybind>
    <keybind key="W-2"><action name="GoToDesktop"><to>2</to></action></keybind>
  </keyboard>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="",
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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    client = None

    def logtext():
        log.flush()
        return open(log_path).read()

    def pointer(*args):
        subprocess.run(["wlrctl", "pointer", *args], env=cenv, capture_output=True)

    def move(dx, dy):
        pointer("move", str(dx), str(dy))

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True)
        time.sleep(0.5)

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def motions(lines):
        return [tuple(map(int, m.groups())) for l in lines
                if (m := re.match(r"motion (-?\d+) (-?\d+)", l))]

    def rel_sum(lines):
        total = [0, 0]
        for l in lines:
            if m := re.match(r"rel (-?\d+) (-?\d+)", l):
                total[0] += int(m.group(1))
                total[1] += int(m.group(2))
        return tuple(total)

    try:
        client = Client(client_bin, cenv)
        line, seen = client.wait_for("configured")
        if line is None:
            raise RuntimeError(f"client never configured: {seen}")
        time.sleep(0.5)
        m = re.search(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        if m is None:
            raise RuntimeError("window never mapped")
        w, h, x, y = map(int, m.groups())

        # Pointer to the window centre.
        move(-9000, -9000)
        move(x + w // 2, y + h // 2)
        got = client.drain(1.0)
        positions = [tuple(map(int, m.groups())) for l in got
                     if (m := re.match(r"(?:enter|motion) (-?\d+) (-?\d+)", l))]
        if not positions:
            raise RuntimeError(f"pointer never reached the window: {got}")
        ex = positions[-1][0]  # where the cursor is, surface-local

        # 1. Lock: relative motion only; the cursor stays put.
        client.send("lock")
        line, seen = client.wait_for("locked")
        expect(line is not None, f"lock not activated: {seen}")
        move(200, 0)
        got = client.drain()
        expect(rel_sum(got)[0] >= 150, f"no relative motion while locked: {got}")
        expect(not motions(got), f"pointer motion while locked: {got}")
        client.send("release")
        client.wait_for("released")
        move(1, 0)
        got = client.drain()
        mv = motions(got)
        expect(bool(mv) and abs(mv[-1][0] - (ex + 1)) <= 2,
               f"cursor moved while locked (expected x≈{ex + 1}): {got}")

        # 2. Confine: cannot leave the window.
        client.send("confine")
        line, seen = client.wait_for("confined")
        expect(line is not None, f"confine not activated: {seen}")
        move(-5000, 0)
        move(0, -5000)
        got = client.drain()
        expect("leave" not in got, f"pointer left a confined window: {got}")
        mv = motions(got)
        expect(bool(mv) and mv[-1][0] >= 0 and mv[-1][1] >= 0,
               f"confined pointer outside the window: {got}")
        client.send("release")
        client.wait_for("released")

        # 3. Keyboard focus: a desktop switch releases the lock.
        move(-9000, -9000)
        move(x + w // 2, y + h // 2)
        client.drain()
        client.send("lock")
        line, seen = client.wait_for("locked")
        expect(line is not None, f"lock not activated again: {seen}")
        keys("-M", "logo", "-k", "2", "-m", "logo")
        line, seen = client.wait_for("unlocked", timeout=2)
        expect(line is not None, f"desktop switch did not release the lock: {seen}")
        keys("-M", "logo", "-k", "1", "-m", "logo")
        line, seen = client.wait_for("locked", timeout=2)
        expect(line is not None, f"lock not restored on return: {seen}")
        client.send("release")
        client.wait_for("released")

        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if client is not None:
            client.close()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        log.close()

    contents = open(log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration pointer lock: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
