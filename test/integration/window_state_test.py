#!/usr/bin/env python3
"""Headless integration test for the window-state model.

Drives a real Waybox via the virtual-keyboard protocol through a
maximize -> fullscreen -> un-fullscreen -> un-maximize sequence and checks,
from the compositor's geometry log, that each state restores to the right
rectangle. This specifically guards the separate per-state restore rects:
with a single shared restore rect, un-maximizing after a fullscreen round
trip would restore the *maximized* geometry instead of the original
floating one.

Skips (exit 77) when foot/wtype or a headless display are unavailable.

Usage: window_state_test.py <waybox-binary>
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

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="A-m"><action name="ToggleMaximize"/></keybind>
    <keybind key="A-f"><action name="Fullscreen"/></keybind>
  </keyboard>
</openbox_config>
"""


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    need("foot")
    need("wtype")

    workdir = tempfile.mkdtemp(prefix="waybox-ws-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(RC_XML)

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

    def key(combo):
        # combo like ("alt", "m")
        mod, k = combo
        subprocess.run(["wtype", "-M", mod, "-k", k, "-m", mod],
                       env=cenv, capture_output=True, text=True)

    rc = 0
    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(2.5)
        key(("alt", "m"))   # maximize
        time.sleep(0.8)
        key(("alt", "f"))   # fullscreen
        time.sleep(0.8)
        key(("alt", "f"))   # un-fullscreen -> should restore maximized geom
        time.sleep(0.8)
        key(("alt", "m"))   # un-maximize -> should restore original floating geom
        time.sleep(0.8)
        foot.terminate()
        time.sleep(1.0)
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

    # Parse the geometry transitions: "wb-geom <tag> WxH+X+Y"
    geom = {}
    for tag, w, h, x, y in re.findall(
            r"wb-geom (\S+) (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", contents):
        geom.setdefault(tag, []).append((int(w), int(h), int(x), int(y)))

    def first(tag):
        return geom.get(tag, [None])[0]

    for tag in ("mapped", "maximize-on", "fullscreen-on", "fullscreen-off",
                "maximize-off"):
        if first(tag) is None:
            print(f"FAIL: missing '{tag}' transition in log", file=sys.stderr)
            sys.stderr.write(contents)
            return 1

    # un-fullscreen restores the maximized geometry
    if first("fullscreen-off") != first("maximize-on"):
        print(f"FAIL: fullscreen-off {first('fullscreen-off')} != "
              f"maximize-on {first('maximize-on')}", file=sys.stderr)
        rc = 1
    # un-maximize restores the original floating geometry (the bug this guards)
    if first("maximize-off") != first("mapped"):
        print(f"FAIL: maximize-off {first('maximize-off')} != "
              f"mapped {first('mapped')}", file=sys.stderr)
        rc = 1

    if rc == 0:
        shutil.rmtree(workdir, ignore_errors=True)
        print("integration window_state: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
