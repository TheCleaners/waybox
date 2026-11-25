#!/usr/bin/env python3
"""Integration test for virtual desktops (rc.xml <desktops>, GoToDesktop,
SendToDesktop, ToggleOmnipresent).

Two foot windows with distinct background colours ("one" red, "two" blue),
checked by sampling the screen with grim:

1. "one" opens on desktop 1. Going to desktop 2 hides it; "two" opens there.
2. Back on desktop 1, "one" shows and has focus again, and Alt+Tab does not
   reach "two" (it is on another desktop).
3. SendToDesktop 3 with <follow>no</follow> hides "one" and stays on
   desktop 1; focusing it from a taskbar (wlrctl) goes to desktop 3.
4. Reconfiguring down to 2 desktops moves "one" and the view to desktop 2.
5. ToggleOmnipresent shows "one" on every desktop.

Skips (exit 77) without foot, wtype, wlrctl, grim, magick or a headless
display.

Usage: desktops_test.py <waybox-binary>
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
RED = (255, 0, 0)
BLUE = (0, 0, 255)


def rc_xml(desktops):
    keys = "".join(
        f'<keybind key="W-{n}"><action name="GoToDesktop"><to>{n}</to></action></keybind>'
        for n in (1, 2, 3))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>{desktops}</number></desktops>
  <keyboard>
    {keys}
    <keybind key="W-S-3">
      <action name="SendToDesktop"><to>3</to><follow>no</follow></action>
    </keybind>
    <keybind key="W-o"><action name="ToggleOmnipresent"/></keybind>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
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
    for tool in ("foot", "wtype", "wlrctl", "grim", "magick"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-desktops-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(rc_xml(3))
    foot_ini = {}
    for name, colour in (("one", "ff0000"), ("two", "0000ff")):
        foot_ini[name] = os.path.join(workdir, f"foot-{name}.ini")
        with open(foot_ini[name], "w") as f:
            f.write(f"[colors-dark]\nbackground={colour}\nforeground={colour}\n")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)

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
    feet = []

    def logtext():
        log.flush()
        return open(log_path).read()

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True)
        time.sleep(0.5)

    def super_key(k, shift=False):
        if shift:
            keys("-M", "logo", "-M", "shift", "-k", k, "-m", "shift", "-m", "logo")
        else:
            keys("-M", "logo", "-k", k, "-m", "logo")

    def open_foot(name):
        before = len(re.findall(r"wb-geom mapped", logtext()))
        feet.append(subprocess.Popen(
            ["foot", "-c", foot_ini[name], "-a", name], env=cenv,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for _ in range(40):
            time.sleep(0.1)
            found = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
                               logtext())
            if len(found) > before:
                w, h, x, y = map(int, found[-1])
                time.sleep(0.5)  # first frame
                return (x + w // 2, y + h // 2)
        failures.append(f"foot {name} never mapped")
        return None

    def colour_at(point):
        x, y = point
        out = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"],
                             env=cenv, capture_output=True, check=True).stdout
        txt = subprocess.run(["magick", "ppm:-", "-format",
                              "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]",
                              "info:-"], input=out, capture_output=True,
                             check=True).stdout.decode()
        return tuple(int(v) for v in txt.split(","))

    def focused():
        m = re.findall(r"Keyboard focus is now on surface: (\S+)", logtext())
        return m[-1] if m else None

    def desktop_switches():
        return re.findall(r"wb-desktop (\d+)$", logtext(), re.M)

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        one = open_foot("one")
        if one is None:
            return 1
        expect(colour_at(one) == RED, f"one not shown on desktop 1: {colour_at(one)}")

        # 1. Desktop 2 hides "one"; "two" opens there.
        super_key("2")
        expect(desktop_switches()[-1:] == ["2"], "no switch to desktop 2 logged")
        expect(colour_at(one) != RED, "one still shown on desktop 2")
        two = open_foot("two")
        if two is None:
            return 1
        expect(focused() == "two", f"two not focused on map ({focused()})")

        # 2. Back to desktop 1: "one" again, focused; Alt+Tab stays here.
        super_key("1")
        expect(colour_at(one) == RED, "one not shown again on desktop 1")
        expect(colour_at(two) != BLUE, "two shown on desktop 1")
        expect(focused() == "one", f"one not refocused on desktop 1 ({focused()})")
        keys("-M", "alt", "-k", "Tab", "-m", "alt")
        expect(focused() == "one", f"Alt+Tab left desktop 1 ({focused()})")
        expect(desktop_switches()[-1:] == ["1"], "Alt+Tab switched desktops")

        # 3. Send "one" away without following; a taskbar request follows it.
        super_key("3", shift=True)
        expect(colour_at(one) != RED, "one still shown after SendToDesktop 3")
        expect(desktop_switches()[-1:] == ["1"], "SendToDesktop followed the window")
        subprocess.run(["wlrctl", "toplevel", "focus", "app_id:one"], env=cenv,
                       capture_output=True)
        time.sleep(0.5)
        expect(desktop_switches()[-1:] == ["3"], "activating one did not go to desktop 3")
        expect(colour_at(one) == RED, "one not shown on desktop 3")
        expect(focused() == "one", f"one not focused on desktop 3 ({focused()})")

        # 4. Fewer desktops: windows and the view move to the last one.
        with open(rc_path, "w") as f:
            f.write(rc_xml(2))
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.8)
        expect(desktop_switches()[-1:] == ["2"], "view not moved off removed desktop 3")
        expect(colour_at(one) == RED, "one not shown after reconfigure")

        # 5. Omnipresent: "one" shows on desktop 1 too.
        super_key("o")
        super_key("1")
        expect(desktop_switches()[-1:] == ["1"], "no switch to desktop 1")
        expect(colour_at(one) == RED, "omnipresent one not shown on desktop 1")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for f in feet:
            if f.poll() is None:
                f.terminate()
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
    print("integration desktops: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
