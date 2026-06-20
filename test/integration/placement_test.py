#!/usr/bin/env python3
"""Headless integration test for smart window placement.

Opens several windows and checks the compositor's geometry log shows them at
distinct positions (rather than stacked at the same origin), i.e. Smart
placement / cascade is actually moving them.

Skips (exit 77) when foot or a headless display are unavailable.

Usage: placement_test.py <waybox-binary> <rc.xml>
"""
import os
import re
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
    waybox, rc_xml = sys.argv[1], sys.argv[2]
    if shutil.which("foot") is None:
        print("SKIP: foot not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-pl-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    shutil.copyfile(rc_xml, os.path.join(cfg, "rc.xml"))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        ASAN_OPTIONS="detect_leaks=1:exitcode=99",
    )

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)

    display = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        for name in os.listdir(runtime):
            if name.startswith("wayland-") and not name.endswith(".lock"):
                display = name
                break
        if display:
            break
    if display is None:
        log.close()
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    clients = []
    rc = 0
    try:
        for _ in range(3):
            clients.append(subprocess.Popen(["foot"], env=cenv,
                                            stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL))
            time.sleep(1.8)
    finally:
        for c in clients:
            c.terminate()
        time.sleep(1.0)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        print("FAIL: sanitizer/assert output", file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    positions = re.findall(r"wb-geom mapped \d+x\d+\+(-?\d+)\+(-?\d+)", contents)
    if len(positions) < 3:
        print(f"FAIL: expected 3 mapped windows, saw {len(positions)}",
              file=sys.stderr)
        sys.stderr.write(contents)
        return 1
    if len(set(positions)) < 3:
        print(f"FAIL: windows were not placed at distinct positions: {positions}",
              file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print(f"integration placement: OK ({positions})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
