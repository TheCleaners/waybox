#!/usr/bin/env python3
"""Integration test: wbconf's color control and its eyedropper.

A headless Waybox with the scripting socket, its background red on the
left half and blue on the right. wbconf's bell color's eyedropper
(WBCONF_TEST_SET colorpick.waybox.bell.color=1) asks Waybox for a pick;
a click on the left half answers it; saved with Ctrl+S, rc.xml has
<bell color="#ff0000">. (wbconf waits for the click on another thread:
its window keeps working meanwhile.)
Skips (exit 77) without wtype or wlrctl, or a headless display.

Usage: wbconf_color_test.py <waybox> <wbconf> <schema-dir> <desktop_client>
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
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, desktop = sys.argv[1:]
    for tool in ("wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbcolor-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><ipc enabled="yes"/><virtualInput allow="always"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
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
    bg = subprocess.Popen([desktop, "background", "halves"], env=cenv,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wbtest.reap_on_exit(bg)
    time.sleep(0.8)
    wenv = dict(cenv, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid), WBCONF_TEST_SET="colorpick.waybox.bell.color=1")
    app = None
    try:
        app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        for _ in range(60):
            time.sleep(0.1)
            if "wb-pick start" in open(log_path).read():
                break
        else:
            raise RuntimeError("wbconf's eyedropper didn't start a pick")
        # The red half: well left of wbconf's window, which opens at the left
        # edge... so the bottom left corner, under no window.
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "20", "700"], env=cenv, capture_output=True)
        time.sleep(0.3)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        time.sleep(1.0)
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(1.5)
        rc = open(rc_path).read()
        if not re.search(r'<bell[^>]*color="#ff0000"', rc):
            failures.append(f"the picked color wasn't saved:\n{rc}")
    except (RuntimeError, OSError) as e:
        failures.append(str(e))
    finally:
        if app is not None and app.poll() is None:
            app.kill()
        bg.kill()
        bg.wait()
        if proc.poll() is None:
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
    print("integration wbconf color: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
