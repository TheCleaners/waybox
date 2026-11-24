#!/usr/bin/env python3
"""Integration test for the window menu (ShowMenu client-menu).

Two desktops and framed foot windows (decorations forced on by an app
rule). The menu is driven with the keyboard; its entries are, with
separators skipped: Send to desktop, Iconify, Maximize, Move, Resize,
Roll up, Decorate, Close.

1. Right-clicking the titlebar opens it; Maximize maximizes the window.
2. Roll up hides the window's contents (the desktop shows through) without
   resizing it, and Roll down brings them back.
3. Send to desktop > 2 sends the window to desktop 2.
4. Alt+Space opens it with the pointer elsewhere; Close closes the window.
5. A window that closes while its menu is open takes the menu with it:
   keys pressed afterwards don't act on another window.

Skips (exit 77) without foot, wlrctl, wtype, grim, magick or a headless
display.

Usage: window_menu_test.py <waybox-binary>
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
    for tool in ("foot", "wlrctl", "wtype", "grim", "magick"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-winmenu-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="A-space">
      <action name="ShowMenu"><menu>client-menu</menu></action>
    </keybind>
    <keybind key="W-1"><action name="GoToDesktop"><to>1</to></action></keybind>
  </keyboard>
  <mouse>
    <context name="Titlebar">
      <mousebind button="Right" action="Press">
        <action name="ShowMenu"><menu>client-menu</menu></action>
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
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
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
    feet = []

    def logtext():
        log.flush()
        return open(log_path).read()

    def pointer(*args, pause=0.2):
        subprocess.run(["wlrctl", "pointer", *args], env=cenv, capture_output=True)
        time.sleep(pause)

    def go_to(x, y):
        pointer("move", "-9000", "-9000", pause=0)
        pointer("move", str(x), str(y))

    def key(*names, mods=()):
        args = []
        for m in mods:
            args += ["-M", m]
        for n in names:
            args += ["-k", n]
        for m in mods:
            args += ["-m", m]
        subprocess.run(["wtype", *args], env=cenv, capture_output=True)
        time.sleep(0.3)

    def open_foot(app_id):
        before = len(re.findall(r"wb-geom mapped", logtext()))
        p = subprocess.Popen(["foot", "-a", app_id], env=cenv,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        feet.append(p)
        for _ in range(40):
            time.sleep(0.1)
            found = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
                               logtext())
            if len(found) > before:
                time.sleep(0.5)
                return p, tuple(map(int, found[-1]))
        raise RuntimeError(f"{app_id} never mapped")

    def colour_at(px, py):
        out = subprocess.run(["grim", "-g", f"{px},{py} 1x1", "-t", "ppm", "-"],
                             env=cenv, capture_output=True, check=True).stdout
        txt = subprocess.run(["magick", "ppm:-", "-format",
                              "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]",
                              "info:-"], input=out, capture_output=True,
                             check=True).stdout.decode()
        return tuple(int(v) for v in txt.split(","))

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        _, (w, _, x, y) = open_foot("foot")
        title = (x + w // 3, y - 6)

        # 1. Right-click the titlebar; Maximize.
        go_to(*title)
        pointer("click", "right", pause=0.4)
        key("Down", "Down", "Down", "Return")  # Send to, Iconify, Maximize
        time.sleep(0.5)
        expect("wb-geom maximize-on" in logtext(),
               "Maximize from the titlebar's window menu did not maximize")

        # 2. Roll up and down.
        m = re.findall(r"wb-geom maximize-on (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        if not m:
            raise RuntimeError("no maximized geometry")
        mw, mh, mx, my = map(int, m[-1])
        centre = (mx + mw // 2, my + mh // 2)
        black = (0, 0, 0)
        expect(colour_at(*centre) != black, "window contents not drawn")
        go_to(mx + mw // 3, my - 6)
        pointer("click", "right", pause=0.4)
        key(*(["Down"] * 6), "Return")  # ... Resize, Roll up
        time.sleep(0.5)
        shade = re.findall(r"wb-geom shade-on (\d+)x(\d+)", logtext())
        expect(bool(shade) and tuple(map(int, shade[-1])) == (mw, mh),
               f"Roll up did not roll up, or resized the window: {shade}")
        expect(colour_at(*centre) == black,
               f"rolled-up window still shows its contents: {colour_at(*centre)}")
        go_to(mx + mw // 3, my - 6)
        pointer("click", "right", pause=0.4)
        key(*(["Down"] * 6), "Return")  # ... Roll down
        time.sleep(0.5)
        expect("wb-geom shade-off" in logtext(), "Roll down did not roll down")
        expect(colour_at(*centre) != black, "contents not back after Roll down")

        # 3. Send to desktop > 2.
        go_to(mx + mw // 3, my - 6)
        pointer("click", "right", pause=0.4)
        key("Down", "Right")    # Send to desktop; opening selects desktop 1
        key("Down", "Return")   # desktop 2
        time.sleep(0.5)
        expect("wb-desktop send foot to 2" in logtext(),
               "Send to desktop 2 from the window menu did nothing")
        key("1", mods=("logo",))

        # 4. Alt+Space with the pointer elsewhere; Close.
        b, (bw, bh, bx, by) = open_foot("second")
        go_to(bx + bw + 300, by + bh + 200)  # off the window
        key("space", mods=("alt",))
        key(*(["Down"] * 8), "Return")       # ... Decorate, Close
        for _ in range(30):
            if b.poll() is not None:
                break
            time.sleep(0.1)
        expect(b.poll() is not None, "Close from Alt+Space's window menu did not close it")

        # 5. The window closes while its menu is open.
        d, _ = open_foot("fourth")  # the one that must survive
        c, (cw, _, cx, cy) = open_foot("third")  # on top: its titlebar is clear
        go_to(cx + cw // 3, cy - 6)
        pointer("click", "right", pause=0.4)  # the menu for "third"
        c.terminate()
        c.wait()
        time.sleep(0.5)
        key(*(["Down"] * 8), "Return")
        time.sleep(0.8)
        expect(d.poll() is None,
               "after its window closed, the window menu acted on another window")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for p in feet:
            if p.poll() is None:
                p.terminate()
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
    print("integration window menu: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
