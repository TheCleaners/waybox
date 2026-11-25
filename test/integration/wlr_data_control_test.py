#!/usr/bin/env python3
"""Integration test: clipboard managers on the older wlr-data-control
protocol (cliphist, clipman) see the clipboard.

A client speaking only zwlr_data_control watches the clipboard while
wl-copy sets it twice; it sees both texts.

Skips (exit 77) without wl-copy, or a headless display.

Usage: wlr_data_control_test.py <waybox> <wlr_dc_client>
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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    if shutil.which("wl-copy") is None:
        print("SKIP: wl-copy not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wlrdc-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=os.path.join(workdir, "config"),
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
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
    client = subprocess.Popen([client_bin], env=cenv, stdout=subprocess.PIPE, text=True)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l.strip()) for l in client.stdout],
                     daemon=True).start()

    def wait_for(want, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if line == want:
                return True
        return False

    copiers = []
    try:
        if not wait_for("ready"):
            raise RuntimeError("the clipboard manager couldn't bind zwlr_data_control")
        for text in ("first copy", "second copy"):
            # wl-copy stays to serve the selection until replaced.
            copiers.append(subprocess.Popen(["wl-copy", text], env=cenv,
                                            stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL))
            if not wait_for(f"selection {text}"):
                failures.append(f"the clipboard manager didn't see {text!r}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for c in copiers + [client]:
            if c.poll() is None:
                c.kill()
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
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wlr data control: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
