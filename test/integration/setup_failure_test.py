#!/usr/bin/env python3
"""Integration test: a window's setup failing partway (as when memory runs
out) is undone, and the next try works.

WAYBOX_TEST_FAIL=toplevel,capture makes each of those steps fail once:

1. The first window's scene tree "can't be made": that client is refused
   (no_memory), Waybox goes on; a second window opens normally.
2. The first capture of that window "can't be made": grim gets nothing;
   asked again, it captures it (a capture that failed isn't kept failed).

Skips (exit 77) without foot or grim with -T, a headless display, or
wlroots 0.20.

Usage: setup_failure_test.py <waybox>
"""
import os
import re
import shutil
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
    for tool in ("foot", "grim"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    if "-T" not in subprocess.run(["grim", "-h"], capture_output=True, text=True).stdout + \
            subprocess.run(["grim", "-h"], capture_output=True, text=True).stderr:
        print("SKIP: grim has no -T", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-setupfail-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               WAYBOX_TEST_FAIL="toplevel,capture",
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
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    if sock is None:
        log.close()
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    cenv.pop("WAYBOX_TEST_FAIL", None)
    failures = []
    kids = []

    def text():
        log.flush()
        return open(log_path).read()

    def settle(check, timeout=5.0):
        end = time.time() + timeout
        while not check():
            if time.time() >= end:
                return False
            time.sleep(0.1)
        return True

    try:
        if "Window capture needs wlroots 0.20" in text():
            print("SKIP: built against wlroots < 0.20", file=sys.stderr)
            return SKIP
        # 1. The first window refused; Waybox on; the next one fine.
        first = subprocess.Popen(["foot", "-a", "first", "sleep", "60"], env=cenv,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kids.append(first)
        if not settle(lambda: "wb-test-fail toplevel" in text()):
            failures.append("the first window's setup wasn't made to fail")
        if not settle(lambda: first.poll() is not None):
            failures.append("the refused client wasn't told (it still runs)")
        second = subprocess.Popen(["foot", "-a", "second", "sleep", "60"], env=cenv,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kids.append(second)
        if not settle(lambda: re.search(r"wb-toplevel second id (\S+)", text())):
            failures.append("the second window didn't open")
        else:
            ident = re.findall(r"wb-toplevel second id (\S+)", text())[-1]
            time.sleep(0.5)
            out = os.path.join(workdir, "win.ppm")

            def grab():
                if os.path.exists(out):
                    os.unlink(out)
                r = subprocess.run(["grim", "-t", "ppm", "-T", ident, out], env=cenv,
                                   capture_output=True, text=True, timeout=10)
                return r.returncode == 0 and os.path.exists(out) and os.path.getsize(out) > 0
            # 2. The first capture fails; the next works.
            if grab():
                failures.append("the capture made to fail captured anyway")
            if "wb-test-fail capture" not in text():
                failures.append("the capture wasn't made to fail")
            if not grab():
                failures.append("after one failed, the window couldn't be captured again")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for k in kids:
            if k.poll() is None:
                k.kill()
            k.wait()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    t = open(log_path).read()
    if any(s in t for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(t[-2500:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration setup_failure: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
