#!/usr/bin/env python3
"""Integration test: clipboard managers on the older wlr-data-control
protocol (cliphist, clipman) see the clipboard.

A client speaking only zwlr_data_control watches the clipboard while
wl-copy sets it twice; it sees both texts. A primary selection is set
too. Then, with the screen locked:
- a copy is refused (the manager doesn't see it), a primary one too, and
  clearing the clipboard as well;
- what was copied before the lock can still be read (by a reader bound
  before it, as clipboard managers are: decided in #397);
- the primary selection's source exits.
After unlocking, the clipboard is what it was before the lock, and the
primary selection is empty (its source gone), not stale.

Skips (exit 77) without wl-copy, or a headless display.

Usage: wlr_data_control_test.py <waybox> <wlr_dc_client> <lock_client>
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
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, lock_bin = sys.argv[1:]
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

        primary = None
        if shutil.which("wl-paste"):
            primary = subprocess.Popen(["wl-copy", "--foreground", "--primary", "primary before"], env=cenv,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            copiers.append(primary)
            time.sleep(0.4)

        # Locked: a copy is refused (by any client: wl-copy uses data
        # control as clipboard managers do); unlocked, the clipboard is as
        # it was.
        locker = subprocess.Popen([lock_bin], env=cenv, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(locker)
        for _ in range(30):
            if "locked" in (locker.stdout.readline() or ""):
                break
        copiers.append(subprocess.Popen(["wl-copy", "set while locked"], env=cenv,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        if wait_for("selection set while locked", timeout=1.5):
            failures.append("a copy made while the screen was locked was taken")
        if shutil.which("wl-paste"):
            def paste(*args):
                r = subprocess.run(["wl-paste", "-n", *args], env=cenv, capture_output=True,
                                   text=True, timeout=5)
                return r.stdout if r.returncode == 0 else None
            # Still readable (copied before the lock).
            if paste() != "second copy":
                failures.append(f"while locked, the clipboard read {paste()!r}")
            if paste("--primary") != "primary before":
                failures.append(f"while locked, the primary selection read {paste('--primary')!r}")
            # A primary copy and a clear: refused too.
            copiers.append(subprocess.Popen(["wl-copy", "--primary", "primary locked"],
                                            env=cenv, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL))
            subprocess.run(["wl-copy", "--clear"], env=cenv, capture_output=True, timeout=5)
            time.sleep(0.5)
            if paste("--primary") != "primary before":
                failures.append("a primary copy made while locked was taken")
            if paste() != "second copy":
                failures.append("the clipboard was cleared while locked")
            # The primary selection's source goes while locked.
            primary.kill()
            primary.wait()
            time.sleep(0.3)
        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        locker.wait(timeout=5)
        time.sleep(0.3)
        if shutil.which("wl-paste"):
            pasted = subprocess.run(["wl-paste", "-n"], env=cenv, capture_output=True,
                                    text=True, timeout=5).stdout
            if pasted != "second copy":
                failures.append(f"after unlocking, the clipboard is {pasted!r}, not the one "
                                "from before the lock")
            r = subprocess.run(["wl-paste", "-n", "--primary"], env=cenv, capture_output=True,
                               text=True, timeout=5)
            if r.returncode == 0 and r.stdout:
                failures.append(f"after its source went, the primary selection is {r.stdout!r}")
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
