#!/usr/bin/env python3
"""Integration test: two outputs side by side.

Regression test for layer surfaces and window placement on an output that
is not at the layout origin (a second monitor):

1. A panel (layer_client.c, a 30px bar on every output) must be drawn on
   the second output, not stacked on the first. Layer surfaces are arranged
   in output-local coordinates, so the output's layer trees must sit at the
   output's layout position.
2. A window opened with the pointer on the second output opens there,
   below that output's panel.
3. Maximizing it fills the second output's usable area (layout
   coordinates), not the first output's.

Skips (exit 77) without foot, wlrctl, wtype, grim, magick or a headless
display.

Usage: multi_output_test.py <waybox-binary> <layer_client-binary>
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
PANEL = 30          # layer_client.c BAR_HEIGHT


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, layer_client = sys.argv[1], sys.argv[2]
    for tool in ("foot", "wlrctl", "wtype", "grim", "magick", "wlr-randr"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-multi-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-Up"><action name="ToggleMaximize"/></keybind>
  </keyboard>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2",
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
    time.sleep(0.3)  # both outputs up

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    panel = foot = None

    def logtext():
        log.flush()
        return open(log_path).read()

    def colour_at(x, y):
        out = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"],
                             env=cenv, capture_output=True, check=True).stdout
        txt = subprocess.run(["magick", "ppm:-", "-format",
                              "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]",
                              "info:-"], input=out, capture_output=True,
                             check=True).stdout.decode()
        return tuple(int(v) for v in txt.split(","))

    def is_panel(c):
        return c[0] == c[1] == c[2] and 0x18 <= c[0] <= 0x38

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        outs = subprocess.run(["wlr-randr"], env=cenv, capture_output=True,
                              text=True).stdout
        positions = [tuple(map(int, p)) for p in
                     re.findall(r"Position: (-?\d+),(-?\d+)", outs)]
        sizes = re.findall(r"(\d+)x(\d+) px[^\n]*current", outs)
        if len(positions) < 2 or not sizes:
            raise RuntimeError(f"expected two outputs side by side:\n{outs}")
        OUTPUT_W, OUTPUT_H = map(int, sizes[0])
        if sorted(positions) != [(0, 0), (OUTPUT_W, 0)]:
            raise RuntimeError(f"unexpected output layout {positions}:\n{outs}")

        # 1. A panel on each output.
        panel = subprocess.Popen([layer_client], env=cenv,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        first = colour_at(20, PANEL // 2)
        second = colour_at(OUTPUT_W + 20, PANEL // 2)
        expect(is_panel(first), f"no panel on the first output: {first}")
        expect(is_panel(second),
               f"no panel on the second output (drawn on the first?): {second}")

        # 2. A window opened with the pointer on the second output.
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(OUTPUT_W + 300), "300"],
                       env=cenv, capture_output=True)
        time.sleep(0.3)
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
            failures.append("foot never mapped")
        else:
            w, _, x, y = geom
            expect(OUTPUT_W <= x and x + w <= 2 * OUTPUT_W,
                   f"window not placed on the second output: {geom}")
            expect(y >= PANEL, f"window under the second output's panel: {geom}")

            # 3. Maximize fills the second output's usable area.
            time.sleep(0.5)
            subprocess.run(["wtype", "-M", "logo", "-k", "Up", "-m", "logo"],
                           env=cenv, capture_output=True)
            time.sleep(0.8)
            m = re.search(r"wb-geom maximize-on (\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
                          logtext())
            if m is None:
                failures.append("maximize did not happen")
            else:
                mw, mh, mx, my = map(int, m.groups())
                expect(mx >= OUTPUT_W and mx + mw <= 2 * OUTPUT_W,
                       f"maximized onto the wrong output: {mw}x{mh}+{mx}+{my}")
                expect(my >= PANEL and my + mh <= OUTPUT_H,
                       f"maximized outside the usable area: {mw}x{mh}+{mx}+{my}")
                expect(mw > OUTPUT_W // 2,
                       f"maximized window is too small: {mw}x{mh}+{mx}+{my}")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for p in (foot, panel):
            if p is not None and p.poll() is None:
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
    print("integration multi-output: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
