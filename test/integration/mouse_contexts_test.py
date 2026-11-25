#!/usr/bin/env python3
"""Integration test for mouse binding contexts and events.

With a framed window (foot, decorations forced on by an app rule):

1. Titlebar + Left + DoubleClick maximizes (two quick clicks); a single
   click does not.
2. Titlebar + Middle + Press runs its action (SendToDesktop without
   following, which logs).
3. Touchpad-style scrolling on the empty desktop (all wlrctl can send) is
   not a wheel notch: it doesn't run the Root + Down binding. (Counting
   wheel notches is covered by the mousebind unit test.)

Skips (exit 77) without foot, wlrctl or a headless display.

Usage: mouse_contexts_test.py <waybox-binary>
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


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wlrctl"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-mousectx-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>3</number></desktops>
  <mouse>
    <context name="Titlebar">
      <mousebind button="Left" action="DoubleClick">
        <action name="ToggleMaximize"/>
      </mousebind>
      <mousebind button="Middle" action="Press">
        <action name="SendToDesktop"><to>2</to><follow>no</follow></action>
      </mousebind>
    </context>
    <context name="Root">
      <mousebind button="Down" action="Click">
        <action name="GoToDesktop"><to>next</to></action>
      </mousebind>
    </context>
  </mouse>
  <applications>
    <application class="foot"><decor>yes</decor></application>
  </applications>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
    foot = None

    def logtext():
        log.flush()
        return open(log_path).read()

    def pointer(*args, pause=0.2):
        subprocess.run(["wlrctl", "pointer", *args], env=cenv, capture_output=True)
        time.sleep(pause)

    def go_to(x, y):
        pointer("move", "-9000", "-9000", pause=0)
        pointer("move", str(x), str(y))

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        geom = None
        for _ in range(40):
            time.sleep(0.1)
            m = re.search(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
            if m:
                geom = tuple(map(int, m.groups()))
                break
        if geom is None:
            raise RuntimeError("foot never mapped")
        w, _, x, y = geom
        time.sleep(0.5)
        title = (x + w // 3, y - 6)  # on the titlebar, clear of the buttons

        # 1. A single click doesn't maximize; a double-click does.
        go_to(*title)
        pointer("click", "left", pause=0.8)  # longer than a double-click
        expect("maximize-on" not in logtext(), "a single titlebar click maximized")
        pointer("click", "left", pause=0.05)
        pointer("click", "left", pause=0.6)
        expect("wb-geom maximize-on" in logtext(),
               "double-click on the titlebar did not maximize")
        # Back to normal size for the next step.
        m = re.findall(r"wb-geom maximize-on (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        if m:
            mw, _, mx, my = map(int, m[-1])
            go_to(mx + mw // 3, my - 6)
            pointer("click", "left", pause=0.05)
            pointer("click", "left", pause=0.6)
            expect("wb-geom maximize-off" in logtext(),
                   "a second double-click did not restore the window")

        # 2. Middle press on the titlebar.
        go_to(*title)
        pointer("click", "middle", pause=0.5)
        expect("wb-desktop send foot to 2" in logtext(),
               "middle press on the titlebar did not run its action")

        # 3. Finger scrolling on the empty desktop is not a wheel notch.
        n = len(re.findall(r"wb-desktop (\d+)$", logtext(), re.M))
        go_to(5, 5)  # the window went to desktop 2; desktop 1 is empty here
        pointer("scroll", "10", "0", pause=0.5)
        switches = re.findall(r"wb-desktop (\d+)$", logtext(), re.M)
        expect(len(switches) == n,
               f"touchpad scrolling ran a wheel binding: {switches[n:]}")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if foot is not None and foot.poll() is None:
            foot.terminate()
        time.sleep(0.5)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
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
    print("integration mouse contexts: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
