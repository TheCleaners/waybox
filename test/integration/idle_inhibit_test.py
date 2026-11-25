#!/usr/bin/env python3
"""Integration test: idle stays inhibited while any inhibitor is alive, and
ends when the last one goes, whatever the order (a video player and a
browser both keeping the screen on).

Inhibitors A and B are made; B is dropped (still inhibited), then A (no
longer inhibited).

Usage: idle_inhibit_test.py <waybox> <inhibit_client>
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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-idle-", dir="/tmp")
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

    def state():
        log.flush()
        found = re.findall(r"wb-idle inhibited (\d)", open(log_path).read())
        return found[-1] if found else None

    failures = []
    client = subprocess.Popen([client_bin], env=dict(env, WAYLAND_DISPLAY=sock),
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l.strip()) for l in client.stdout],
                     daemon=True).start()

    def send(cmd, want):
        client.stdin.write(cmd + "\n")
        client.stdin.flush()
        try:
            while lines.get(timeout=5) != want:
                pass
        except queue.Empty:
            raise RuntimeError(f"the client didn't answer {cmd!r}")
        time.sleep(0.2)

    try:
        if lines.get(timeout=5) != "ready":
            raise RuntimeError("the client didn't connect")
        send("new", "made 0")
        send("new", "made 1")
        if state() != "1":
            failures.append("not inhibited with two inhibitors")
        send("del 1", "deleted 1")
        if state() != "1":
            failures.append("dropping one of two inhibitors ended the inhibition")
        send("del 0", "deleted 0")
        if state() != "0":
            failures.append("still inhibited after the last inhibitor went")
    except (RuntimeError, queue.Empty) as e:
        failures.append(str(e) or "the client didn't connect")
    finally:
        client.kill()
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
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration idle inhibit: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
