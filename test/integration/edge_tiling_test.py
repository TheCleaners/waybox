#!/usr/bin/env python3
"""Integration test for edge tiling (drag to an edge, SnapToEdge).

1. Keyboard, on a Wayland window (foot): Super+Left tiles it to the left
   half, Super+Down restores its geometry, Super+Up maximizes it.
2. Dragging, on an X11 window (x11_client.py starts the move the way a
   titlebar does): at the left edge a translucent preview in the theme's
   colour shows the left half; dropping tiles the window there; dragging it
   away again restores its size.

The second part skips without Xwayland or python-xlib; the whole test skips
(exit 77) without foot, wlrctl, wtype, grim, magick, wlr-randr or a
headless display.

Usage: edge_tiling_test.py <waybox-binary>
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

THEMERC = """
window.active.title.bg: flat solid
window.active.title.bg.color: #ff0000
window.active.label.bg: flat solid
window.active.label.bg.color: #ff0000
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
    for tool in ("foot", "wlrctl", "wtype", "grim", "magick", "wlr-randr"):
        need(tool)
    have_x11 = shutil.which("Xwayland") is not None
    try:
        import Xlib  # noqa: F401
    except ImportError:
        have_x11 = False

    workdir = tempfile.mkdtemp(prefix="waybox-tile-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    data = os.path.join(workdir, "data")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(data, "themes", "TestRed", "openbox-3"))
    with open(os.path.join(data, "themes", "TestRed", "openbox-3", "themerc"), "w") as f:
        f.write(THEMERC)
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><name>TestRed</name></theme>
  <waybox><edgeTiling enabled="yes" range="10"/></waybox>
  <keyboard>
    <keybind key="W-Left"><action name="SnapToEdge"><direction>left</direction></action></keybind>
    <keybind key="W-Up"><action name="SnapToEdge"><direction>up</direction></action></keybind>
    <keybind key="W-Down"><action name="SnapToEdge"><direction>down</direction></action></keybind>
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

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    foot = client = None
    lines = queue.Queue()

    def logtext():
        log.flush()
        return open(log_path).read()

    def geoms(tag):
        return [tuple(map(int, m)) for m in re.findall(
            rf"wb-geom {tag} (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())]

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True)
        time.sleep(0.6)

    def pointer(*args):
        subprocess.run(["wlrctl", "pointer", *args], env=cenv, capture_output=True)
        time.sleep(0.2)

    def colour_at(x, y):
        out = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"],
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
        outs = subprocess.run(["wlr-randr"], env=cenv, capture_output=True,
                              text=True).stdout
        size = re.search(r"(\d+)x(\d+) px[^\n]*current", outs)
        if size is None:
            raise RuntimeError(f"no output size:\n{outs}")
        ow, oh = map(int, size.groups())
        half = ow // 2

        # 1. Keyboard on a Wayland window.
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        for _ in range(40):
            time.sleep(0.1)
            if geoms("mapped"):
                break
        if not geoms("mapped"):
            raise RuntimeError("foot never mapped")
        mapped = geoms("mapped")[-1]
        time.sleep(0.5)
        keys("-M", "logo", "-k", "Left", "-m", "logo")
        tiles = geoms("tile-left")
        if not tiles:
            failures.append("Super+Left did not tile")
        else:
            w, h, x, y = tiles[-1]
            expect(0 <= x < 20 and half - 20 <= x + w <= half,
                   f"left tile not the left half: {tiles[-1]} (half={half})")
        keys("-M", "logo", "-k", "Down", "-m", "logo")
        untiles = geoms("untile")
        expect(bool(untiles) and untiles[-1] == mapped,
               f"Super+Down did not restore {mapped}: {untiles}")
        keys("-M", "logo", "-k", "Up", "-m", "logo")
        expect(bool(geoms("maximize-on")), "Super+Up did not maximize")
        keys("-M", "logo", "-k", "Down", "-m", "logo")
        foot.terminate()
        foot.wait()
        foot = None
        time.sleep(0.5)

        # 2. Dragging an X11 window to the left edge.
        m = re.search(r"Xwayland on DISPLAY=(:\d+)", logtext())
        if not have_x11 or m is None:
            print("note: skipping the drag part (no Xwayland/python-xlib)",
                  file=sys.stderr)
        else:
            client = subprocess.Popen(
                [sys.executable,
                 os.path.join(os.path.dirname(__file__), "x11_client.py")],
                env=dict(env, DISPLAY=m.group(1)), stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, text=True)

            def reader(stream):
                for line in stream:
                    lines.put(line.strip())
            threading.Thread(target=reader, args=(client.stdout,),
                             daemon=True).start()

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

            n_mapped = len(geoms("mapped"))
            send("window drag 400 300 0000ff")
            expect(wait_line("mapped drag") is not None, "X11 window never mapped")
            # Xwayland can be slow to hand the window over (software rendering
            # on a busy CI runner): wait for Waybox to map it, and don't go on
            # with another window's geometry if it never does.
            for _ in range(100):
                time.sleep(0.1)
                if len(geoms("mapped")) > n_mapped:
                    break
            if len(geoms("mapped")) <= n_mapped:
                raise RuntimeError("Waybox never mapped the X11 window (Xwayland too slow?)")
            w, h, x, y = geoms("mapped")[-1]
            time.sleep(0.5)

            def start_move(px, py):
                pointer("move", "-9000", "-9000")
                pointer("move", str(px), str(py))
                send("move drag")
                time.sleep(0.5)

            # Drag to the middle of the left edge.
            start_move(x + 50, y + 50)
            pointer("move", "-9000", "0")
            pointer("move", "2", str(oh // 2 - (y + 50)))
            # The preview covers the left half; sample it above the window.
            probe = colour_at(half - 40, 40)
            expect(probe[0] > 40 and probe[1] < 30 and probe[2] < 30,
                   f"no preview in the theme colour over the left half: {probe}")
            expect(colour_at(ow - 40, 40)[0] < 30, "preview outside the left half")
            n_tiles = len(geoms("tile-left"))
            pointer("click", "left")  # drop
            time.sleep(0.5)
            tiles = geoms("tile-left")
            expect(len(tiles) > n_tiles, "dropping on the left edge did not tile")
            if len(tiles) > n_tiles:
                tw, th, tx, ty = tiles[-1]
                expect(tx < 20 and half - 20 <= tx + tw <= half,
                       f"dropped tile not the left half: {tiles[-1]}")
            expect(colour_at(ow - 40, 40)[0] < 30, "preview still shown after the drop")

            # Drag it away: the size comes back.
            n_untile = len(geoms("untile"))
            start_move(100, oh // 2)
            pointer("move", "300", "0")
            pointer("click", "left")
            time.sleep(0.5)
            untiles = geoms("untile")
            expect(len(untiles) > n_untile and untiles[-1][:2] == (w, h),
                   f"dragging away did not restore {w}x{h}: {untiles[n_untile:]}")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for p in (foot, client):
            if p is not None and p.poll() is None:
                p.kill()
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
    print("integration edge tiling: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
