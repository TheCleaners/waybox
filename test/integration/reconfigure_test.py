#!/usr/bin/env python3
"""Integration test: reconfigure (SIGUSR2, as wbconf sends) restyles windows
that are already open, and programs Waybox starts get a normal signal mask.

1. Open foot with a server-side frame under the built-in theme.
2. Switch rc.xml to a test theme whose active titlebar is solid red and send
   SIGUSR2. The existing window's titlebar must turn red (frames used to keep
   the theme they were created with).
3. Run an Execute binding that records the child's blocked-signal mask: it
   must be empty (Waybox blocks the signals it handles on its event loop, and
   must not pass that on to programs it starts).

Skips (exit 77) without foot, wtype, grim, magick or a headless display.

Usage: reconfigure_test.py <waybox-binary>
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
RED = (255, 0, 0)

THEMERC = """
window.active.title.bg: flat solid
window.active.title.bg.color: #ff0000
window.active.label.bg: flat solid
window.active.label.bg.color: #ff0000
window.active.label.text.color: #ff0000
window.active.border.color: #ff0000
"""


def rc_xml(theme, sigmask_out):
    theme_xml = f"<theme><name>{theme}</name></theme>" if theme else ""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  {theme_xml}
  <keyboard>
    <keybind key="W-x">
      <action name="Execute"><execute>if grep -q "^SigBlk:" /proc/self/status 2&gt;/dev/null; then sed -n "s/^SigBlk:[[:space:]]*//p" /proc/self/status; else ps -o sigmask= -p $$; fi &gt; {sigmask_out}</execute></action>
    </keybind>
  </keyboard>
  <applications>
    <application class="foot"><decor>yes</decor></application>
  </applications>
</openbox_config>
"""


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def strip_colors(env, x, y, w):
    """Distinct colours (with counts) in a 1px-high strip of the screen."""
    out = subprocess.run(["grim", "-g", f"{x},{y} {w}x1", "-t", "ppm", "-"],
                         env=env, capture_output=True, check=True).stdout
    txt = subprocess.run(["magick", "ppm:-", "-format", "%c", "histogram:info:-"],
                         input=out, capture_output=True, check=True).stdout.decode()
    colors = {}
    for m in re.finditer(r"(\d+):\s*\(\s*(\d+),\s*(\d+),\s*(\d+)", txt):
        colors[(int(m.group(2)), int(m.group(3)), int(m.group(4)))] = int(m.group(1))
    return colors


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wtype", "grim", "magick"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-reconf-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    data = os.path.join(workdir, "data")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(data, "themes", "TestRed", "openbox-3"))
    with open(os.path.join(data, "themes", "TestRed", "openbox-3", "themerc"), "w") as f:
        f.write(THEMERC)
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    sigmask_out = os.path.join(workdir, "sigmask")
    with open(rc_path, "w") as f:
        f.write(rc_xml(None, sigmask_out))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=data, XDG_RUNTIME_DIR=runtime,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)

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

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    foot = None
    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(1.8)
        m = re.search(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
                      open(log_path).read())
        if m is None:
            failures.append("foot never mapped")
            return 1
        w, x, y = int(m.group(1)), int(m.group(3)), int(m.group(4))
        # A strip through the middle of the titlebar, above the client area,
        # away from the buttons on the right.
        strip = (x + 4, y - 6, max(20, w // 2))
        before = strip_colors(cenv, *strip)
        if RED in before:
            failures.append(f"titlebar already red before reconfiguring ({before})")

        with open(rc_path, "w") as f:
            f.write(rc_xml("TestRed", sigmask_out))
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.8)
        if proc.poll() is not None:
            failures.append(f"waybox exited on SIGUSR2 (status {proc.returncode})")
        else:
            after = strip_colors(cenv, *strip)
            dominant = max(after, key=lambda c: after[c]) if after else None
            if dominant != RED:
                failures.append(f"existing titlebar not restyled: {after}")

            subprocess.run(["wtype", "-M", "logo", "-k", "x", "-m", "logo"],
                           env=cenv, capture_output=True)
            time.sleep(0.8)
            mask = open(sigmask_out).read().split() if os.path.exists(sigmask_out) else []
            if len(mask) != 1 or int(mask[0], 16) != 0:
                failures.append(f"spawned program has blocked signals: {mask}")
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
    print("integration reconfigure: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
