#!/usr/bin/env python3
"""Integration test for X11 programs under Xwayland.

Uses x11_client.py (python-xlib) for solid-colour X11 windows, a theme with
a solid red active titlebar, and grim to check what is on screen:

1. Programs Waybox starts get DISPLAY.
2. An X11 window maps with focus and a server-side frame.
3. A dialog (WM_TRANSIENT_FOR) opens centred over its parent.
4. An override-redirect window (a menu) is drawn where the client put it,
   and disappears when destroyed.
5. Desktops hide and show X11 windows.
6. After the window is moved, the X server knows its new position (X11
   programs place their own menus from it).
7. A fullscreen request is honoured, and Close sends WM_DELETE_WINDOW.

Skips (exit 77) without Xwayland, python-xlib, wtype, grim, magick or a
headless display.

Usage: xwayland_test.py <waybox-binary>
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
RED = (255, 0, 0)
BLUE = (0, 0, 255)
GREEN = (0, 255, 0)
YELLOW = (255, 255, 0)

THEMERC = """
window.active.title.bg: flat solid
window.active.title.bg.color: #ff0000
window.active.label.bg: flat solid
window.active.label.bg.color: #ff0000
window.active.label.text.color: #ff0000
window.active.border.color: #ff0000
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
    for tool in ("Xwayland", "wtype", "wlrctl", "grim", "magick"):
        need(tool)
    try:
        import Xlib  # noqa: F401
    except ImportError:
        print("SKIP: python-xlib not installed", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-xw-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    data = os.path.join(workdir, "data")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(data, "themes", "TestRed", "openbox-3"))
    with open(os.path.join(data, "themes", "TestRed", "openbox-3", "themerc"), "w") as f:
        f.write(THEMERC)
    display_out = os.path.join(workdir, "display")
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><name>TestRed</name></theme>
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="W-1"><action name="GoToDesktop"><to>1</to></action></keybind>
    <keybind key="W-2"><action name="GoToDesktop"><to>2</to></action></keybind>
    <keybind key="A-F4"><action name="Close"/></keybind>
    <keybind key="W-x">
      <action name="Execute"><execute>sh -c 'echo "$DISPLAY" &gt; {display_out}'</execute></action>
    </keybind>
  </keyboard>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=data, XDG_RUNTIME_DIR=runtime,
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

    def logtext():
        log.flush()
        return open(log_path).read()

    m = re.search(r"Xwayland on DISPLAY=(:\d+)", logtext())
    if m is None:
        log.close()
        sys.stderr.write(open(log_path).read())
        print("SKIP: Xwayland not set up (wlroots without Xwayland?)", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    xdisplay = m.group(1)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    xenv = dict(env, DISPLAY=xdisplay)
    failures = []
    client = None
    lines = queue.Queue()

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True)
        time.sleep(0.6)

    def colour_at(x, y):
        out = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"],
                             env=cenv, capture_output=True, check=True).stdout
        txt = subprocess.run(["magick", "ppm:-", "-format",
                              "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]",
                              "info:-"], input=out, capture_output=True,
                             check=True).stdout.decode()
        return tuple(int(v) for v in txt.split(","))

    def send(cmd):
        client.stdin.write(cmd + "\n")
        client.stdin.flush()

    def wait_line(prefix, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if line.startswith(prefix):
                return line
        return None

    def focused():
        found = re.findall(r"Keyboard focus is now on surface: (\S+)", logtext())
        return found[-1] if found else None

    def last_mapped():
        found = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        return tuple(map(int, found[-1])) if found else None

    try:
        # 1. DISPLAY reaches programs Waybox starts.
        keys("-M", "logo", "-k", "x", "-m", "logo")
        got = open(display_out).read().strip() if os.path.exists(display_out) else None
        expect(got == xdisplay, f"Execute saw DISPLAY={got!r}, expected {xdisplay}")

        client = subprocess.Popen(
            [sys.executable, os.path.join(os.path.dirname(__file__), "x11_client.py")],
            env=xenv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        def reader(stream):
            for line in stream:
                lines.put(line.strip())
        threading.Thread(target=reader, args=(client.stdout,), daemon=True).start()

        # 2. A managed window: mapped, focused, framed.
        send("window main 400 300 0000ff")
        expect(wait_line("mapped main") is not None, "X11 window never mapped")
        time.sleep(1.0)
        geom = last_mapped()
        if geom is None:
            raise RuntimeError("no wb-geom mapped for the X11 window")
        w, h, x, y = geom
        cx, cy = x + w // 2, y + h // 2
        expect(focused() == "main", f"X11 window not focused ({focused()})")
        expect(colour_at(cx, cy) == BLUE, f"X11 window not drawn: {colour_at(cx, cy)}")
        expect(colour_at(x + 20, y - 6) == RED,
               f"no server-side titlebar on the X11 window: {colour_at(x + 20, y - 6)}")

        # 3. A dialog opens centred over its parent.
        send("window dlg 120 80 00ff00 main")
        expect(wait_line("mapped dlg") is not None, "dialog never mapped")
        time.sleep(1.0)
        expect(colour_at(cx, cy) == GREEN,
               f"dialog not centred over its parent: {colour_at(cx, cy)}")
        expect(focused() == "dlg", f"dialog not focused ({focused()})")

        # 4. Override-redirect (menu) windows: drawn at their own position.
        send("override menu 900 500 60 40 ffff00")
        expect(wait_line("mapped menu") is not None, "menu never mapped")
        time.sleep(0.8)
        expect(colour_at(930, 520) == YELLOW, f"menu not drawn: {colour_at(930, 520)}")
        send("destroy menu")
        time.sleep(0.8)
        expect(colour_at(930, 520) != YELLOW, "menu still drawn after destroy")

        # 5. Desktops.
        keys("-M", "logo", "-k", "2", "-m", "logo")
        expect(colour_at(cx, cy) not in (BLUE, GREEN), "X11 windows shown on desktop 2")
        keys("-M", "logo", "-k", "1", "-m", "logo")
        expect(colour_at(cx, cy) == GREEN, "X11 windows not back on desktop 1")

        # 6. A moved window learns its new position.
        def where(name):
            send(f"where {name}")
            line = wait_line(f"where {name}")
            return tuple(map(int, line.split()[2:4])) if line else None
        before = where("main")
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x + 50), str(y + 50)],
                       env=cenv, capture_output=True)
        time.sleep(0.3)
        send("move main")
        time.sleep(0.5)
        subprocess.run(["wlrctl", "pointer", "move", "150", "80"], env=cenv,
                       capture_output=True)
        time.sleep(0.3)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)  # ends the move
        time.sleep(0.5)
        after = where("main")
        if before is None or after is None:
            failures.append(f"no position from the client ({before}, {after})")
        else:
            dx, dy = after[0] - before[0], after[1] - before[1]
            # Within edge-snapping distance (16px) of the pointer's motion.
            expect(abs(dx - 150) <= 20 and abs(dy - 80) <= 20,
                   f"X server position not updated after a move: {before} -> {after}")

        # 7. Fullscreen request; Close asks the client to close.
        send("fullscreen main")
        time.sleep(1.0)
        expect("wb-geom fullscreen-on" in logtext(), "fullscreen request ignored")
        keys("-M", "alt", "-k", "F4", "-m", "alt")
        expect(wait_line("delete") is not None, "Close did not send WM_DELETE_WINDOW")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if client is not None and client.poll() is None:
            client.kill()
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
    print("integration xwayland: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
