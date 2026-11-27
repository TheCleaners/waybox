#!/usr/bin/env python3
"""Integration test: every global Waybox offers is classified.

Sandboxed programs (security-context-v1) get only the globals classified
as ordinary (waybox/security.cpp); one not classified at all is refused
to them and logged ("wb-global unclassified ..."). wayland-info binds
every global a headless Waybox offers: none may be unclassified, so a
protocol added without a decision is caught here, not in a sandbox.

Skips (exit 77) without wayland-info or a headless display.

Usage: protocol_inventory_test.py <waybox>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wayland-info") is None:
        print("SKIP: wayland-info not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-inv-")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=workdir, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
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
        socks = [n for n in os.listdir(runtime) if n.startswith("wayland-")
                 and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    if sock is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    failures = []
    seen = []
    try:
        out = subprocess.run(["wayland-info"], env=dict(env, WAYLAND_DISPLAY=sock),
                             capture_output=True, text=True, timeout=20).stdout
        seen = sorted(set(re.findall(r"interface: '([a-z0-9_]+)'", out)))
        if len(seen) < 40:
            failures.append(f"only {len(seen)} globals listed: {seen}")
        time.sleep(0.3)
        unclassified = re.findall(r"wb-global unclassified ([a-z0-9_]+)", open(log_path).read())
        if unclassified:
            failures.append("globals offered but not classified in waybox/security.cpp "
                            f"(sandboxed programs don't get them): {sorted(set(unclassified))}")
    except subprocess.TimeoutExpired:
        failures.append("wayland-info didn't finish")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    text = open(log_path).read()
    if any(m in text for m in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print(f"integration protocol_inventory: OK ({len(seen)} globals)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
