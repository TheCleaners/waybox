#!/usr/bin/env python3
"""Integration test: the lock screen gets the keyboard even when a program
had a popup menu open with a grab (where the password would leak).

1. popup_client opens a popup with a grab (a click, as a context menu).
2. lock_client locks the session.
3. Typing (virtual input allowed while locked for the test): the keys
   reach the lock screen, none reach the program, and its popup is
   dismissed.

Skips (exit 77) without wtype or wlrctl, or a headless display.

Usage: lock_popup_test.py <waybox> <popup_client> <lock_client>
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
import wbtest

SKIP = 77


class Client:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

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

    def drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.seen.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, popup_bin, lock_bin = sys.argv[1:]
    for tool in ("wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-lockpop-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><virtualInput allow="always"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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
        app = Client([popup_bin], cenv)
        clients.append(app)
        if not app.wait_for("configured"):
            raise RuntimeError("the program's window never showed")
        time.sleep(0.4)
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "100", "100"], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        if not app.wait_for("popup-shown"):
            raise RuntimeError("the program's popup never showed")
        time.sleep(0.3)

        locker = Client([lock_bin], cenv)
        clients.append(locker)
        if not locker.wait_for("locked"):
            raise RuntimeError("the session never locked")
        time.sleep(0.3)
        subprocess.run(["wtype", "secret"], env=cenv, capture_output=True)
        time.sleep(0.6)
        app.drain(0.3)
        locker.drain(0.3)
        app_keys = app.seen.count("key")
        lock_keys = locker.seen.count("key")
        if app_keys:
            failures.append(f"the program got {app_keys} keys typed at the lock screen")
        if lock_keys == 0:
            failures.append("the lock screen got no keys")
        if "popup-done" not in app.seen:
            failures.append("the program's popup wasn't dismissed when locking")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for c in clients:
            if c.proc.poll() is None:
                c.proc.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()

    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lock with a popup grab: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
