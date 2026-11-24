#!/usr/bin/env python3
"""Integration test for the Shade (S) and All desktops (D) titlebar buttons.

With <titleLayout>SDLIMC</titleLayout> the two sit at the left end of the
titlebar. Clicking:

1. D puts the window on all desktops; clicking it again takes it back.
2. S rolls the window up to its titlebar; clicking it again (on the rolled-
   up titlebar) rolls it back down.

Skips (exit 77) without foot or wlrctl, or a headless display.

Usage: titlebar_buttons_test.py <waybox-binary>
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


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-tbtn-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><titleLayout>SDLIMC</titleLayout></theme>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    foot = None

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def logtext():
        log.flush()
        return open(log_path).read()

    def click(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        time.sleep(0.5)

    try:
        foot = subprocess.Popen(["foot", "-a", "tb"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        geo = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        if not geo:
            raise RuntimeError("foot never mapped")
        _, _, x, y = map(int, geo[-1])
        # The titlebar is just above the client; its buttons are roughly
        # 12-16 px with a few px of padding: S centred near x+11, D near x+27.
        bar_y = y - 9
        s_x, d_x = x + 11, x + 27

        # 1. All desktops on, then off.
        click(d_x, bar_y)
        expect("wb-desktop omnipresent tb on" in logtext(),
               "the D button didn't put the window on all desktops")
        click(d_x, bar_y)
        expect("wb-desktop omnipresent tb off" in logtext(),
               "a second click on D didn't take it back")

        # 2. Roll up, then down.
        click(s_x, bar_y)
        expect("wb-geom shade-on" in logtext(), "the S button didn't roll the window up")
        click(s_x, bar_y)
        expect("wb-geom shade-off" in logtext(), "a second click on S didn't roll it down")
        expect(proc.poll() is None, "waybox exited")
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
        log.close()

    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration titlebar buttons: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
