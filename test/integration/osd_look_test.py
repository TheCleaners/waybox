#!/usr/bin/env python3
"""Integration test: the on-screen displays' look and place.

A made-up theme with Openbox's osd.* keys (osd.bg.color green, no border)
and rc.xml's <waybox><osd position="bottom"/>; the scripting socket's
"osd" request shows a display (wbconf's Test), its settings over rc.xml's.
Read from the screen (grim), down the middle of the 1280x720 output:
1. Nothing over: green (the theme's osd.bg, not its menu's), near the
   bottom (rc.xml), nothing in the middle. So is the desktop's name when
   switching desktops (the display itself, not the preview).
2. position=top, bg red: red near the top.
3. bg blue, cornerRadius 20: its corner left clear; with 0, blue there.
4. opacity 50: blue mixed with what's under it.
5. opacity 5 (out of range): an error, nothing shown.
Skips (exit 77) without grim, or a headless display.

Usage: osd_look_test.py <waybox> <waybox-msg>
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77
W, H = 1280, 720

THEMERC = """menu.items.bg: flat solid
menu.items.bg.color: #dddddd
menu.items.text.color: #000000
border.width: 0
osd.bg: flat solid
osd.bg.color: #00ff00
osd.label.text.color: #00ff00
osd.border.width: 0
"""


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, msg = sys.argv[1:]
    if shutil.which("grim") is None:
        print("SKIP: grim not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-osdlook-", dir="/tmp")
    share = os.path.join(workdir, "share")
    tdir = os.path.join(share, "themes", "OsdTest", "openbox-3")
    os.makedirs(tdir)
    with open(os.path.join(tdir, "themerc"), "w") as f:
        f.write(THEMERC)
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><name>OsdTest</name></theme>
  <waybox><ipc enabled="yes"/><osd position="bottom"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, XDG_DATA_DIRS=share,
               XDG_DATA_HOME=os.path.join(workdir, "data"), WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0")
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def ask(req):
        out = subprocess.run([msg, "raw", json.dumps(req)], env=cenv, capture_output=True,
                             text=True, timeout=10)
        try:
            return json.loads(out.stdout)
        except ValueError:
            return {"raw": out.stdout + out.stderr}

    def screen():
        r = subprocess.run(["grim", "-t", "ppm", "-"], env=cenv, capture_output=True,
                           timeout=10)
        data = r.stdout
        # P6\n1280 720\n255\n
        parts = data.split(b"\n", 3)
        w, h = (int(v) for v in parts[1].split())
        return w, h, parts[3]

    def px(shot, x, y):
        w, _, data = shot
        i = (y * w + x) * 3
        return tuple(data[i:i + 3])

    def rows_where(shot, test):
        """y values down the middle column where test(pixel) holds."""
        w, h, _ = shot
        return [y for y in range(h) if test(px(shot, w // 2, y))]

    def show(req):
        r = ask(dict({"request": "osd", "message": "Sample"}, **req))
        time.sleep(0.4)
        return r, screen()

    green = lambda p: p[1] > 200 and p[0] < 60 and p[2] < 60
    red = lambda p: p[0] > 200 and p[1] < 60 and p[2] < 60
    blue = lambda p: p[2] > 200 and p[0] < 60 and p[1] < 60
    try:
        r, shot = show({})
        ys = rows_where(shot, green)
        if r.get("ok") is not True or not ys:
            failures.append(f"nothing green shown: {r} (middle {px(shot, W // 2, H // 2)})")
        elif min(ys) < H * 0.6:
            failures.append(f"not near the bottom: rows {min(ys)}..{max(ys)}")
        time.sleep(2.2)

        r = ask({"request": "action", "action": "GoToDesktop", "to": 2})
        time.sleep(0.4)
        shot = screen()
        ys = rows_where(shot, green)
        if not ys or min(ys) < H * 0.6:
            failures.append(f"the desktop name: {r}, green rows {ys[:1]}..{ys[-1:]}")
        time.sleep(1.5)

        r, shot = show({"position": "top", "bg": "#ff0000"})
        ys = rows_where(shot, red)
        if not ys or max(ys) > H * 0.4:
            failures.append(f"position=top, red: rows {ys[:1]}..{ys[-1:]}: {r}")
        time.sleep(2.0)

        # Corners: the box's edges from the middle row and column.
        for radius, clear in (("20", True), ("0", False)):
            r, shot = show({"position": "center", "bg": "#0000ff", "cornerRadius": radius})
            ys = rows_where(shot, blue)
            if not ys:
                failures.append(f"cornerRadius {radius}: nothing blue: {r}")
                continue
            y0, y1 = min(ys), max(ys)
            ym = (y0 + y1) // 2
            xs = [x for x in range(W) if blue(px(shot, x, ym))]
            x0 = min(xs)
            corner = px(shot, x0 + 1, y0 + 1)
            if blue(corner) == clear:
                failures.append(f"cornerRadius {radius}: corner {corner} at {x0 + 1},{y0 + 1}")
            time.sleep(2.0)

        r, shot = show({"position": "center", "bg": "#0000ff", "opacity": "50"})
        p = px(shot, W // 2, H // 2)
        # Some blue, not all of it: mixed with the desktop under it.
        if not (60 < p[2] < 230) or blue(p):
            failures.append(f"opacity 50: {p}")
        time.sleep(2.0)

        r = ask({"request": "osd", "opacity": "5"})
        # waybox-msg prints an error as text, not JSON.
        if "not an on-screen display setting: opacity=5" not in r.get("raw", "") and \
                "error" not in r:
            failures.append(f"opacity 5 accepted: {r}")
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration osd look: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
