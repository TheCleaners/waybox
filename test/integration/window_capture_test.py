#!/usr/bin/env python3
"""Integration test for window capture (ext-foreign-toplevel-image-capture-
source), as used to share a single window.

A foot window with a solid blue background is captured with grim -T (by its
ext-foreign-toplevel identifier, which Waybox logs):

1. The capture is the window's size and shows its contents.
2. It still works while the window is hidden on another desktop.
3. Capturing, then closing the window, leaves Waybox running.

Skips (exit 77) without foot, grim with -T, wtype, magick, a headless
display, or wlroots 0.20 (which the protocol needs).

Usage: window_capture_test.py <waybox-binary>
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
BLUE = (0, 0, 255)


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "grim", "wtype", "magick"):
        need(tool)
    if "-T" not in subprocess.run(["grim", "-h"], capture_output=True,
                                  text=True).stdout:
        print("SKIP: grim without -T (window capture)", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-wcap-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="W-2"><action name="GoToDesktop"><to>2</to></action></keybind>
  </keyboard>
</openbox_config>
""")
    foot_ini = os.path.join(workdir, "foot.ini")
    with open(foot_ini, "w") as f:
        f.write("[colors-dark]\nbackground=0000ff\nforeground=0000ff\n")
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

    def logtext():
        log.flush()
        return open(log_path).read()

    if "Window capture needs wlroots 0.20" in logtext():
        proc.kill()
        log.close()
        print("SKIP: built against wlroots < 0.20", file=sys.stderr)
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    foot = None

    def capture(identifier):
        out = os.path.join(workdir, "win.ppm")
        r = subprocess.run(["grim", "-t", "ppm", "-T", identifier, out], env=cenv,
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None, r.stderr.strip()
        size = subprocess.run(["magick", "identify", "-format", "%w %h", out],
                              capture_output=True, text=True).stdout.split()
        centre = subprocess.run(
            ["magick", out, "-gravity", "center", "-crop", "1x1+0+0", "-format",
             "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]", "info:"],
            capture_output=True, text=True).stdout
        return (tuple(map(int, size)), tuple(map(int, centre.split(",")))), ""

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        foot = subprocess.Popen(["foot", "-c", foot_ini, "-a", "capme"], env=cenv,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ident = geom = None
        for _ in range(40):
            time.sleep(0.1)
            m = re.search(r"wb-toplevel capme id (\S+)", logtext())
            g = re.search(r"wb-geom mapped (\d+)x(\d+)", logtext())
            if m and g:
                ident, geom = m.group(1), tuple(map(int, g.groups()))
                break
        if ident is None:
            raise RuntimeError("foot never mapped (or no identifier logged)")
        time.sleep(0.6)

        # 1. The window, at its size.
        got, err = capture(ident)
        expect(got is not None, f"window capture failed: {err}")
        if got:
            size, centre = got
            expect(size == geom, f"capture is {size}, the window {geom}")
            expect(centre == BLUE, f"capture doesn't show the window: {centre}")

        # 2. Hidden on another desktop.
        subprocess.run(["wtype", "-M", "logo", "-k", "2", "-m", "logo"], env=cenv,
                       capture_output=True)
        time.sleep(0.5)
        got, err = capture(ident)
        expect(got is not None and got[1] == BLUE,
               f"capture of a window on another desktop failed: {got or err}")

        # 3. Close it after capturing.
        foot.terminate()
        foot.wait()
        time.sleep(0.5)
        expect(proc.poll() is None, "waybox exited after a captured window closed")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if foot is not None and foot.poll() is None:
            foot.kill()
        time.sleep(0.3)
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
    print("integration window capture: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
