#!/usr/bin/env python3
"""Integration test: the system bell (xdg-system-bell).

1. Without a <bell> setting, a ring is received and does nothing else.
2. With <bell flash="yes" command="...">: a ring from a window flashes
   that window, one with no window flashes the screen; the command runs,
   once for a quick burst of rings.

Usage: bell_test.py <waybox> <bell_client>
"""
import os
import queue
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




def run(waybox, client_bin, workdir, bell_xml):
    """One Waybox with that <bell> (or none); the log and what the
    command left."""
    cfg = os.path.join(workdir, "config")
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{bell_xml}</waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    shutil.rmtree(runtime, ignore_errors=True)
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        proc.kill()
        return None, None
    client = Client([client_bin], dict(env, WAYLAND_DISPLAY=sock), stdin=True)
    problem = None
    try:
        if not client.wait_for("configured"):
            problem = f"the window never showed: {client.seen}"
        else:
            time.sleep(0.3)
            for what in ("window", "window", "window", "none"):
                client.proc.stdin.write(what + "\n")
                client.proc.stdin.flush()
                if not client.wait_for("rang"):
                    problem = "the bell never rang"
                    break
            time.sleep(0.5)
    finally:
        client.proc.kill()
        client.proc.wait()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    contents = open(log_path).read()
    if proc.returncode not in (0, None) or any(
            s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        problem = (problem or "") + f" waybox exit {proc.returncode} / sanitizer output"
    return contents, problem


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-bell-", dir="/tmp")
    failures = []
    marker = os.path.join(workdir, "rung")

    log, problem = run(waybox, client_bin, workdir, "")
    if log is None:
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    if problem:
        failures.append(f"default: {problem}")
    if log.count("wb-bell ring bell-test") != 3 or "wb-bell ring (no window)" not in log:
        failures.append("the rings weren't all received")
    if "wb-bell flash" in log:
        failures.append("the bell flashed without being set to")

    log, problem = run(waybox, client_bin, workdir,
                       f'<bell flash="yes" command="echo x &gt;&gt; {marker}"/>')
    if problem:
        failures.append(f"set: {problem}")
    if log.count("wb-bell flash window") != 3:
        failures.append(f"a ring from a window didn't flash it ({log.count('wb-bell flash window')})")
    if "wb-bell flash output" not in log:
        failures.append("a ring with no window didn't flash the screen")
    lines = open(marker).read().count("x") if os.path.exists(marker) else 0
    if lines < 1:
        failures.append("the bell's command didn't run")
    elif lines > 2:
        failures.append(f"a burst of rings ran the command {lines} times")

    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration bell: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
