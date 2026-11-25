#!/usr/bin/env python3
"""Integration test: fullscreen windows cover panels.

layer_client puts a 30px panel (layer-shell, top layer) on the output and
redraws it every frame; every redraw is a commit, which used to raise the
panel's layer above the fullscreen layer.

1. An ordinary window stays below the panel (the panel is visible).
2. A fullscreen window (foot -F, solid blue) covers the panel, and keeps
   covering it while the panel keeps redrawing.
3. A window opening meanwhile is focused and seen: the fullscreen one is
   stacked with ordinary windows while it isn't active (as in Openbox),
   so the new window shows, and so does the panel.
4. The fullscreen window focused again covers everything again.
5. When the fullscreen window closes, the panel is visible again.

Skips (exit 77) without foot, grim or magick, or a headless display.

Usage: fullscreen_panel_test.py <waybox-binary> <layer_client-binary>
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
BLUE = "srgb(0,0,255)"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, layer_client = sys.argv[1:]
    for tool in ("foot", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-fspanel-", dir="/tmp")
    foot_ini = os.path.join(workdir, "foot.ini")
    with open(foot_ini, "w") as f:
        # Only [colors-dark]: foot versions that deprecate [colors] show a
        # yellow warning bar across the top of the window, where the test
        # looks for the panel.
        f.write("[colors-dark]\nbackground=0000ff\nforeground=0000ff\n")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
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
    children = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def top_pixel():
        """The colour in the middle of the panel's strip."""
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        return subprocess.run(["magick", shot, "-format", "%[pixel:p{200,15}]", "info:"],
                              capture_output=True, text=True).stdout.strip()

    def pixel(x, y):
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        return subprocess.run(["magick", shot, "-format", f"%[pixel:p{{{x},{y}}}]", "info:"],
                              capture_output=True, text=True).stdout.strip()

    def mapped(app_id):
        m = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+) " + app_id,
                       open(log_path).read())
        return tuple(map(int, m[-1])) if m else None

    def foot(*args):
        p = subprocess.Popen(["foot", "-c", foot_ini, *args], env=cenv,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        children.append(p)
        return p

    try:
        bar = subprocess.Popen([layer_client], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        children.append(bar)
        time.sleep(1.0)

        # 1. An ordinary window, maximized: the panel stays visible above it.
        win = foot("-m")
        time.sleep(1.5)
        expect(top_pixel() != BLUE, "a maximized window covers the panel")

        # 2. A fullscreen window covers the panel, while the panel redraws.
        full = foot("-F", "--app-id=full")
        time.sleep(1.5)
        expect(top_pixel() == BLUE, f"the panel shows over a fullscreen window "
               f"({top_pixel()})")
        time.sleep(1.0)  # dozens more panel commits
        expect(top_pixel() == BLUE, "the panel came back over the fullscreen window")

        # 3. Another window opens: focused, and seen over the fullscreen one;
        # the panel shows again.
        other = foot("--app-id=other", "-o", "colors-dark.background=ff0000",
                     "-W", "40x10")
        time.sleep(1.5)
        log.flush()
        g = mapped("other")
        if g is None:
            failures.append("no geometry for the other window")
        else:
            w, h, x, y = g
            got = pixel(x + w // 2, y + h // 2)
            expect(got == "srgb(255,0,0)",
                   f"a window opened over a fullscreen one is hidden under it ({got})")
        expect(top_pixel() != BLUE, "the panel stays hidden while another window is active")

        # 4. The fullscreen window focused again: over everything again.
        subprocess.run(["wlrctl", "toplevel", "focus", "app_id:full"], env=cenv,
                       capture_output=True)
        time.sleep(1.0)
        expect(top_pixel() == BLUE, f"the fullscreen window focused again doesn't cover "
               f"the panel ({top_pixel()})")
        if g is not None:
            got = pixel(x + w // 2, y + h // 2)
            expect(got == "srgb(0,0,255)",
                   f"the fullscreen window focused again is under the other one ({got})")
        other.terminate()

        # 5. Close it: the panel is visible again.
        full.terminate()
        full.wait(timeout=5)
        time.sleep(1.0)
        expect(top_pixel() != BLUE, "the panel is still hidden after fullscreen ended")
        win.terminate()
        expect(proc.poll() is None, "waybox exited")
    except subprocess.TimeoutExpired as e:
        failures.append(str(e))
    finally:
        for c in children:
            if c.poll() is None:
                c.kill()
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
    print("integration fullscreen over panels: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
