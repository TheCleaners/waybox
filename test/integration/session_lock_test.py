#!/usr/bin/env python3
"""Headless integration test for ext-session-lock-v1.

Uses test/integration/lock_client.c, a minimal locker that paints every
output green and unlocks on "unlock" from stdin. Checks that:

  1. a lock takes effect: the screen shows only the locker (green),
     keys typed don't reach the window behind it, and compositor bindings
     (W-S-M ToggleMaximize) don't fire;
  2. a second locker is refused ("finished") while the first is alive;
  3. if the locker dies without unlocking, the session stays locked and
     blank (no window content visible) and keys still go nowhere;
  4. a new locker can take over the dead lock and unlock, after which typing
     reaches the window again;
  5. virtual input (wtype/wlrctl) follows <waybox><virtualInput allow=...>:
     with the default "unlocked" it never reaches the locker; with "always"
     it does (e.g. for on-screen keyboards).

Skips (exit 77) without foot, wtype, wlrctl, grim, magick or a headless
display.

Usage: session_lock_test.py <waybox-binary> <lock-client-binary> [unlocked|always]
"""
import json
import os
import re
import socket
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><virtualInput allow="{policy}"/></waybox>
  <keyboard>
    <keybind key="W-S-M"><action name="ToggleMaximize"/></keybind>
  </keyboard>
</openbox_config>
"""

GREEN = (0, 255, 0)
BLANK = (20, 20, 20)  # session_lock.cpp kBlank, 0.08 * 255


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def screen_colors(env, path):
    """The set of distinct (r, g, b) colours in a screenshot."""
    r = subprocess.run(["grim", path], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        return {("grim failed: " + r.stderr.strip(),)}
    out = subprocess.run(["magick", path, "-unique-colors", "-depth", "8", "txt:-"],
                         capture_output=True, text=True, check=True).stdout
    return {tuple(int(v) for v in m.groups())
            for m in re.finditer(r"\(\s*(\d+),\s*(\d+),\s*(\d+)", out)}


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    waybox, lock_client = sys.argv[1], sys.argv[2]
    policy = sys.argv[3] if len(sys.argv) == 4 else "unlocked"
    for tool in ("foot", "wtype", "wlrctl", "grim", "magick"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-lock-", dir="/tmp")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(RC_XML.format(policy=policy))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=workdir, XDG_RUNTIME_DIR=runtime,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    display = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            display = socks[0]
            break
    if display is None:
        log.close()
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    out = os.path.join(workdir, "typed")
    shot = os.path.join(workdir, "shot.png")
    failures = []
    lockers = []

    def locker():
        p = subprocess.Popen([lock_client], env=cenv, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True)
        lockers.append(p)
        return p

    # Read the locker's stdout with os.read on the raw fd: buffered readline()
    # would pull several lines at once and hide them from select().
    pending = {}

    def read_lines(p, timeout):
        import select
        fd = p.stdout.fileno()
        buf = pending.setdefault(fd, b"")
        lines = []
        deadline = time.time() + timeout
        while True:
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                lines.append(line.decode().strip())
            left = deadline - time.time()
            if left <= 0:
                break
            ready, _, _ = select.select([fd], [], [], left)
            if not ready:
                break
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            buf += chunk
        pending[fd] = buf
        return lines

    def answer(p, timeout=5.0):
        """The locker's next "locked" or "finished" line ("" on timeout)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in read_lines(p, 0.2):
                if line in ("locked", "finished"):
                    return line
        return ""

    def drain(p, timeout=0.6):
        """Lines the locker printed since the last read."""
        return read_lines(p, timeout)

    def type_line(text):
        subprocess.run(["wtype", text], env=cenv, capture_output=True)
        subprocess.run(["wtype", "-k", "Return"], env=cenv, capture_output=True)
        time.sleep(0.5)

    def typed():
        return open(out).read().split() if os.path.exists(out) else []

    foot = subprocess.Popen(
        ["foot", "sh", "-c", f'while read x; do echo "$x" >> {out}; done'],
        env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def steps():
        time.sleep(1.8)
        type_line("before")
        if typed() != ["before"]:
            failures.append(f"setup: typing did not reach foot before locking ({typed()})")

        # The About card, open when the screen locks: closed by the lock,
        # not drawn over it.
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
                c.settimeout(5)
                c.connect(os.path.join(runtime, f"waybox-{display}.sock"))
                c.sendall((json.dumps({"request": "action", "action": "ShowAbout"}) + "\n").encode())
                c.recv(65536)
        except OSError as e:
            failures.append(f"setup: the IPC socket: {e}")
        time.sleep(0.3)

        # 1. Lock.
        first = locker()
        if answer(first) != "locked":
            failures.append("lock: locker did not receive 'locked'")
        time.sleep(0.5)
        colors = screen_colors(cenv, shot)
        if colors != {GREEN}:
            failures.append(f"lock: screen is not just the locker's colour: {sorted(colors)[:6]}")
        type_line("secret")
        subprocess.run(["wtype", "-M", "logo", "-M", "shift", "-k", "m",
                        "-m", "shift", "-m", "logo"], env=cenv, capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "50", "50"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        time.sleep(0.4)
        if "secret" in typed():
            failures.append("lock: keys typed while locked reached the window")

        seen = drain(first)
        if policy == "always":
            if "key" not in seen or "button" not in seen:
                failures.append(f"virtualInput=always: input did not reach the locker ({seen})")
            assert first.stdin is not None
            first.stdin.write("unlock\n")
            first.stdin.flush()
            first.wait(5)
            return  # the rest is covered by the default run
        if "key" in seen or "button" in seen:
            failures.append(f"virtualInput=unlocked: virtual input reached the locker ({seen})")

        # 2. A second locker is refused while the first is alive.
        second = locker()
        if answer(second) != "finished":
            failures.append("lock: a second locker was not refused")

        # 3. The locker dies: stay locked and blank.
        first.send_signal(signal.SIGKILL)
        first.wait()
        time.sleep(0.6)
        colors = screen_colors(cenv, shot)
        if colors != {BLANK}:
            failures.append(f"crash: screen not blank after the locker died: {sorted(colors)[:6]}")
        type_line("leak")
        if "leak" in typed():
            failures.append("crash: keys reached the window after the locker died")

        # 4. A new locker takes over and unlocks.
        third = locker()
        if answer(third) != "locked":
            failures.append("relock: a new locker could not take over the dead lock")
        time.sleep(0.4)
        assert third.stdin is not None
        third.stdin.write("unlock\n")
        third.stdin.flush()
        if third.wait(5) != 0:
            failures.append("unlock: locker did not exit cleanly")
        time.sleep(0.5)
        type_line("after")
        if typed()[-1:] != ["after"]:
            failures.append(f"unlock: typing did not reach foot after unlocking ({typed()})")
        if GREEN in screen_colors(cenv, shot):
            failures.append("unlock: locker content still visible after unlocking")

    try:
        steps()
    finally:
        for p in lockers:
            if p.poll() is None:
                p.kill()
        foot.terminate()
        time.sleep(0.8)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("waybox did not exit on SIGTERM")
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if "maximize-on" in contents:
        failures.append("lock: W-S-M fired while locked")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-4000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration session lock: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
