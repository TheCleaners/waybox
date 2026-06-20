#!/usr/bin/env python3
"""Headless integration test for foreign-toplevel-management (taskbars).

Opens foot, confirms it shows up in the zwlr-foreign-toplevel-management list
(via `wlrctl toplevel list`), then closes it through the protocol
(`wlrctl toplevel close`) and checks the window actually exits -- exercising
handle creation (title/app_id) and the request_close path.

Skips (exit 77) when foot/wlrctl or a headless display are unavailable.

Usage: foreign_toplevel_test.py <waybox-binary> <rc.xml>
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
    waybox, rc_xml = sys.argv[1], sys.argv[2]
    for tool in ("foot", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-ft-")
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
    rc = 0
    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(2.5)

        listing = subprocess.run(["wlrctl", "toplevel", "list"], env=cenv,
                                 capture_output=True, text=True)
        if "foot" not in listing.stdout:
            print(f"FAIL: foot not in toplevel list: {listing.stdout!r}",
                  file=sys.stderr)
            rc = 1

        # Close the window through the management protocol.
        subprocess.run(["wlrctl", "toplevel", "close", "app_id:foot"], env=cenv,
                       capture_output=True, text=True)
        exited = False
        for _ in range(20):
            if foot.poll() is not None:
                exited = True
                break
            time.sleep(0.1)
        if not exited:
            print("FAIL: foot did not exit after wlrctl toplevel close",
                  file=sys.stderr)
            foot.terminate()
            rc = 1
    finally:
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

    if rc == 0:
        shutil.rmtree(workdir, ignore_errors=True)
        print("integration foreign_toplevel: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
