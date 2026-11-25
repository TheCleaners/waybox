#!/usr/bin/env python3
"""Integration test for wl_fixes: offered at version 2; destroy_registry
frees a registry with the connection still fine; ack_global_remove goes
through to libwayland (acknowledging a global that wasn't removed is the
protocol's invalid_ack_remove error).

Usage: fixes_test.py <waybox> <fixes_client>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-fixes-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=os.path.join(workdir, "config"),
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    lp = os.path.join(workdir, "waybox.log")
    log = open(lp, "w")
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    failures = []
    out = subprocess.run([client], env=dict(env, WAYLAND_DISPLAY=sock), capture_output=True,
                         text=True, timeout=10).stdout.split("\n")
    if "version 2" not in out:
        failures.append(f"wl_fixes isn't offered at version 2: {out}")
    if "registry-destroyed" not in out:
        failures.append(f"destroy_registry failed: {out}")
    if "ack-error wl_fixes 0" not in out:
        failures.append(f"ack_global_remove didn't reach libwayland: {out}")
    time.sleep(0.3)
    if proc.poll() is not None:
        failures.append("waybox exited")
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    contents = open(lp).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wl_fixes: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
