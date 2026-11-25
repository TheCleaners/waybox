#!/usr/bin/env python3
"""Headless integration test for keyboard focus, Shift bindings and rule-sized
placement.

1. Keyboard focus without a keyboard: the headless backend starts with no
   keyboard, so a window mapped before any keyboard exists must still get
   wl_keyboard.enter. Text typed afterwards via a virtual keyboard (wtype)
   must reach it.
2. Shift bindings: "W-S-M" (ToggleMaximize) must fire even when the keymap
   reports the unshifted keysym for Shift+m, as wtype's one-level keymap does.
3. Smart placement honours app-rule sizes: two windows that ask for 900x600
   (too wide to fit side by side) but are sized 500x300 by a rule (which do
   fit) must not overlap.

Skips (exit 77) without foot, wtype or a headless display.

Usage: input_focus_test.py <waybox-binary>
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
  <placement><policy>Smart</policy></placement>
  <keyboard>
    <keybind key="W-S-M"><action name="ToggleMaximize"/></keybind>
  </keyboard>
  <applications>
    <application class="foot">
      <size><width>500</width><height>300</height></size>
    </application>
  </applications>
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
    for tool in ("foot", "wtype"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-focus-")
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
        ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99",
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
    out = os.path.join(workdir, "typed")
    clients = []
    failures = []

    try:
        # 1. First window maps with no keyboard on the seat; type into it.
        clients.append(subprocess.Popen(
            ["foot", "--window-size-pixels=900x600", "sh", "-c", f'read x; echo "$x" > {out}; exec sleep 30'],
            env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.8)
        subprocess.run(["wtype", "hello"], env=cenv, capture_output=True)
        subprocess.run(["wtype", "-k", "Return"], env=cenv, capture_output=True)
        time.sleep(0.6)
        typed = open(out).read().strip() if os.path.exists(out) else ""
        if typed != "hello":
            failures.append(f"focus: typed text did not reach the window ({typed!r})")

        # 2. W-S-M via a one-level keymap (wtype reports 'm' with Shift held).
        subprocess.run(["wtype", "-M", "logo", "-M", "shift", "-k", "m",
                        "-m", "shift", "-m", "logo"], env=cenv, capture_output=True)
        time.sleep(0.6)
        subprocess.run(["wtype", "-M", "logo", "-M", "shift", "-k", "m",
                        "-m", "shift", "-m", "logo"], env=cenv, capture_output=True)
        time.sleep(0.6)

        # 3. A second rule-sized window must be placed without overlap.
        clients.append(subprocess.Popen(["foot", "--window-size-pixels=900x600"], env=cenv,
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
            failures.append("waybox did not exit on SIGTERM")
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")

    # The maximize toggles are checked in the geometry log (wlrctl's
    # state:maximized matching misses the maximized state, enum value 0).
    toggles = re.findall(r"wb-geom (maximize-on|maximize-off)", contents)
    if toggles[:2] != ["maximize-on", "maximize-off"]:
        failures.append(f"shift binding: W-S-M did not toggle maximize ({toggles})")

    rects = [tuple(map(int, m)) for m in
             re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", contents)]
    if len(rects) < 2:
        failures.append(f"placement: expected 2 mapped windows, saw {len(rects)}")
    else:
        (w1, h1, x1, y1), (w2, h2, x2, y2) = rects[0], rects[1]
        if (w1, h1) != (500, 300) or (w2, h2) != (500, 300):
            failures.append(f"placement: rule size not applied: {rects[:2]}")
        overlap = x1 < x2 + w2 and x2 < x1 + w1 and y1 < y2 + h2 and y2 < y1 + h1
        if overlap:
            failures.append(f"placement: rule-sized windows overlap: {rects[:2]}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print(f"integration input/focus: OK ({rects[:2]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
