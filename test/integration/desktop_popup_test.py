#!/usr/bin/env python3
"""Integration test for the desktop popup (<desktops><popupTime>).

Three desktops, the first two named; Super+1/2/3 switch:

1. Switching shows the desktop's name in the middle of the monitor, with
   a square per desktop before it, the shown one highlighted (Openbox's
   pager); after popupTime it is gone (the screen is as before).
2. An unnamed desktop shows "Desktop N"; switching again while the popup is
   up replaces it, and it hides once.
3. With popupTime 0 (after Reconfigure), nothing is shown.

Skips (exit 77) without wtype, grim or magick, or a headless display.

Usage: desktop_popup_test.py <waybox-binary>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops>
    <number>3</number>
    <names><name>Mail</name><name>Code</name></names>
    <popupTime>{ms}</popupTime>
  </desktops>
  <keyboard>
    <keybind key="W-1"><action name="GoToDesktop"><to>1</to></action></keybind>
    <keybind key="W-2"><action name="GoToDesktop"><to>2</to></action></keybind>
    <keybind key="W-3"><action name="GoToDesktop"><to>3</to></action></keybind>
  </keyboard>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wtype", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-dpopup-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc, "w") as f:
        f.write(RC.format(ms=700))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def logtext():
        log.flush()
        return open(log_path).read()

    def key(n):
        subprocess.run(["wtype", "-M", "logo", "-k", str(n), "-m", "logo"], env=cenv,
                       capture_output=True)

    def centre():
        """The mean colour of a strip across the middle of the screen."""
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        return subprocess.run(["magick", shot, "-gravity", "center", "-crop", "300x20+0+0",
                               "-format", "%[fx:mean]", "info:"],
                              capture_output=True, text=True).stdout.strip()

    try:
        time.sleep(0.3)
        before = centre()

        # 1. A named desktop.
        key(2)
        time.sleep(0.25)
        expect("wb-desktop-popup Code" in logtext(), "no popup naming desktop 2")
        expect("wb-desktop-popup Code on 1 pager 2/3" in logtext(),
               "the popup has no square per desktop, the second highlighted")
        expect(centre() != before, "the popup isn't drawn in the middle")
        time.sleep(1.0)
        expect(logtext().count("wb-desktop-popup hidden") == 1, "the popup didn't hide")
        expect(centre() == before, "something is left on screen after the popup")

        # 2. Unnamed, then quickly another: replaced, hidden once.
        key(3)
        time.sleep(0.15)
        key(1)
        time.sleep(0.2)
        text = logtext()
        expect("wb-desktop-popup Desktop 3" in text, "no popup for the unnamed desktop")
        expect("wb-desktop-popup Mail" in text, "the second switch didn't update the popup")
        time.sleep(1.0)
        expect(logtext().count("wb-desktop-popup hidden") == 2,
               f"expected one more hide, got {logtext().count('wb-desktop-popup hidden') - 1}")

        # 3. Off.
        with open(rc, "w") as f:
            f.write(RC.format(ms=0))
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        shown = logtext().count("wb-desktop-popup ")
        key(2)
        time.sleep(0.4)
        expect("wb-desktop 2" in logtext().split("Reconfigured")[-1],
               "Super+2 didn't switch after Reconfigure")
        expect(logtext().count("wb-desktop-popup ") == shown,
               "a popup was shown with popupTime 0")
        expect(proc.poll() is None, "waybox exited")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        log.close()

    contents = open(log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration desktop popup: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
