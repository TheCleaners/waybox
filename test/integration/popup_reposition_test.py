#!/usr/bin/env python3
"""Integration test: a popup placed anew (xdg_popup.reposition) is kept on
screen, as at its first commit.

GTK opens a popover at a placeholder size, then repositions it at its real
size; a popover near the screen's edge was left where it fell, off screen.
popup_client opens a popup (a click), then repositions it 100x500 under
its 400x300 window's bottom edge, allowed to flip or slide up: on a 720
pixel tall output that doesn't fit there, so the configure answering the
reposition must have moved it up (y < 299, where it would fall), and
come with the reposition's token.

Skips (exit 77) without wlrctl, or a headless display.

Usage: popup_reposition_test.py <waybox> <popup_client>
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
        for line in self.proc.stdout or []:
            self.lines.put(line.strip())

    def wait_for(self, prefix, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.seen.append(line)
            if line.startswith(prefix):
                return line
        return None

    def send(self, line):
        if self.proc.stdin is not None:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, popup_bin = sys.argv[1:]
    if shutil.which("wlrctl") is None:
        print("SKIP: wlrctl not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-popuprepos-", dir="/tmp")
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
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
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
    app = None
    try:
        app = Client([popup_bin], cenv)
        wbtest.reap_on_exit(app.proc)
        if not app.wait_for("configured"):
            raise RuntimeError("the window never showed")
        time.sleep(0.4)
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "100", "100"], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        if not app.wait_for("popup-shown"):
            raise RuntimeError(f"no popup: {app.seen}")
        app.send("reposition")
        token = app.wait_for("popup-repositioned")
        # The configure answering it follows the token.
        conf = app.wait_for("popup-configure") if token else None
        if token is None or token.split()[1] != "7":
            failures.append(f"no repositioned 7: {app.seen}")
        elif conf is None:
            failures.append(f"no configure after it: {app.seen}")
        else:
            _, y, w, h = (int(v) for v in conf.split()[1:])
            if (w, h) != (100, 500) or y >= 299:
                failures.append(f"repositioned off screen: {conf} (would fall at y=299)")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if app is not None and app.proc.poll() is None:
            app.proc.kill()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration popup reposition: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
