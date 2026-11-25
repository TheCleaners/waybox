#!/usr/bin/env python3
"""Integration test: a panel that starts after a window must not cover it.

Opens foot before any panel exists (so it is placed at the very top of the
output, titlebar included), then starts a panel (layer_client.c) that
reserves 30px at the top. Waybox must move the existing window down by the
reserved height so its server-side titlebar is not underneath the panel.

Skips (exit 77) without foot or a headless display.

Usage: panel_reflow_test.py <waybox-binary> <layer-client-binary>
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
PANEL = 30  # layer_client.c BAR_HEIGHT (exclusive zone)

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <placement><policy>Smart</policy></placement>
  <applications>
    <application class="foot"><decor>yes</decor></application>
  </applications>
</openbox_config>
"""


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, layer_client = sys.argv[1], sys.argv[2]
    if shutil.which("foot") is None:
        print("SKIP: foot not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-reflow-", dir="/tmp")
    os.makedirs(os.path.join(workdir, "waybox"))
    with open(os.path.join(workdir, "waybox", "rc.xml"), "w") as f:
        f.write(RC_XML)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=workdir, XDG_RUNTIME_DIR=runtime,
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
    foot = panel = None
    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(1.8)
        panel = subprocess.Popen([layer_client], env=cenv,
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        time.sleep(1.2)
    finally:
        for p in (panel, foot):
            if p is not None and p.poll() is None:
                p.terminate()
        time.sleep(0.8)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            failures.append("waybox did not exit on SIGTERM")
        log.close()

    contents = open(log_path).read()
    geom = re.compile(r"wb-geom (\S+) (\d+)x(\d+)\+(-?\d+)\+(-?\d+)")
    events = [(m.group(1), int(m.group(5))) for m in geom.finditer(contents)]
    mapped = [y for tag, y in events if tag == "mapped"]
    reflowed = [y for tag, y in events if tag == "reflow"]
    if not mapped:
        failures.append("foot never mapped")
    elif not reflowed:
        failures.append(f"the window was not moved when the panel appeared ({events})")
    elif reflowed[-1] - mapped[0] < PANEL:
        failures.append(f"window moved from y={mapped[0]} to y={reflowed[-1]}; "
                        f"its titlebar is still under the {PANEL}px panel")
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print(f"integration panel reflow: OK (y {mapped[0]} -> {reflowed[-1]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
