#!/usr/bin/env python3
"""Integration test: wbconf's on-screen display settings and their Test.

A headless Waybox with the scripting socket. In wbconf (WBCONF_TEST_SET,
as if changed in the window), the display near the top, its background
red, then Test: a red sample shows near the top of the screen, though
nothing is saved (rc.xml unchanged). Then saved: rc.xml has
<osd position="top" bg="#ff0000">.
Skips (exit 77) without grim, or a headless display.

Usage: wbconf_osd_test.py <waybox> <wbconf> <schema-dir>
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


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("grim") is None:
        print("SKIP: grim not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbosd-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    rc_text = ('<?xml version="1.0" encoding="UTF-8"?>\n'
               '<openbox_config xmlns="http://openbox.org/3.4/rc">\n'
               '  <waybox><ipc enabled="yes"/><virtualInput allow="always"/></waybox>\n'
               '</openbox_config>\n')
    with open(rc_path, "w") as f:
        f.write(rc_text)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
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

    def reds_down_the_middle():
        r = subprocess.run(["grim", "-t", "ppm", "-"], env=cenv, capture_output=True,
                           timeout=10)
        parts = r.stdout.split(b"\n", 3)
        w, h = (int(v) for v in parts[1].split())
        data = parts[3]
        x = w // 2
        ys = []
        for y in range(h):
            i = (y * w + x) * 3
            p = data[i:i + 3]
            if p[0] > 200 and p[1] < 60 and p[2] < 60:
                ys.append(y)
        return ys, h

    wenv = dict(cenv, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_SET="waybox.osd.position=top;waybox.osd.bg=#ff0000;osd-test=1",
                # Saved a few seconds on (after the sample is checked).
                WBCONF_TEST_TOC="dump;dump;dump;dump;save")
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    wbtest.reap_on_exit(app)
    try:
        ys, h = [], 1
        for _ in range(40):
            time.sleep(0.1)
            ys, h = reds_down_the_middle()
            if ys:
                break
        if not ys:
            failures.append("no red sample shown")
        elif max(ys) > h * 0.4:
            failures.append(f"the sample isn't near the top: rows {min(ys)}..{max(ys)} of {h}")
        if open(rc_path).read() != rc_text:
            failures.append("Test saved something")
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        rc = rc_text
        for _ in range(60):
            time.sleep(0.1)
            rc = open(rc_path).read()
            if rc != rc_text:
                break
        if not re.search(r'<osd[^>]*position="top"', rc) or \
                not re.search(r'<osd[^>]*bg="#ff0000"', rc):
            failures.append(f"not saved:\n{rc}")
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
        if app.poll() is None:
            app.kill()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf osd: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
