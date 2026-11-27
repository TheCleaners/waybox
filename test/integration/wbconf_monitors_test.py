#!/usr/bin/env python3
"""Integration test: wbconf's monitor arranger, and keep or revert.

A headless Waybox with two outputs, side by side (HEADLESS-2 left of
HEADLESS-1, as Waybox puts them).
1. HEADLESS-2 dropped below HEADLESS-1 (WBCONF_TEST_SET arrange.NAME=X,Y,
   as a drag would), saved with Ctrl+S, kept: rc.xml has both positions,
   moved to start at 0,0 (HEADLESS-1 0,0 and HEADLESS-2 0,720), and Waybox
   has them there (wlr-randr).
2. HEADLESS-2 dropped to the right of HEADLESS-1, saved, and the question
   left unanswered (WBCONF_KEEP_SECONDS=2): rc.xml has step 1's rules
   again, and Waybox has gone back to them.
Skips (exit 77) without wtype, wlr-randr, or a headless display.

Usage: wbconf_monitors_test.py <waybox> <wbconf> <schema-dir>
"""
import json
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

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox/>
</openbox_config>
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    for tool in ("wtype", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbmon-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid))
    failures = []

    def positions():
        out = subprocess.run(["wlr-randr", "--json"], env=cenv, capture_output=True,
                             text=True, timeout=10).stdout
        return {o["name"]: (o["position"]["x"], o["position"]["y"]) for o in json.loads(out)}

    def outputs_in_rc():
        return dict(re.findall(r'<output name="([^"]+)"[^>]*position="([^"]+)"',
                               open(rc_path).read()))

    def run_wbconf(test_set, extra, wait):
        app = subprocess.Popen([wbconf], env=dict(cenv, WBCONF_TEST_SET=test_set, **extra),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        try:
            time.sleep(4)
            if app.poll() is not None:
                raise RuntimeError(f"wbconf exited ({app.returncode})")
            subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                           capture_output=True, timeout=10)
            time.sleep(wait)
        finally:
            if app.poll() is None:
                app.kill()
            app.wait()

    try:
        start = positions()
        if start.get("HEADLESS-2", (1, 1))[0] >= start.get("HEADLESS-1", (0, 0))[0]:
            raise RuntimeError(f"not the expected start: {start}")

        # 1. Below, kept.
        run_wbconf("arrange.HEADLESS-2=" + "{},{}".format(start["HEADLESS-1"][0], 720),
                   {"WBCONF_TEST_CHOICE": "monitors-keep"}, 1.5)
        want = {"HEADLESS-1": "0,0", "HEADLESS-2": "0,720"}
        if outputs_in_rc() != want:
            failures.append(f"rc.xml after keeping: {outputs_in_rc()}, not {want}")
        now = positions()
        if now != {"HEADLESS-1": (0, 0), "HEADLESS-2": (0, 720)}:
            failures.append(f"Waybox's layout after keeping: {now}")

        # 2. To the right, not answered: back to 1's.
        run_wbconf("arrange.HEADLESS-2=1280,0", {"WBCONF_KEEP_SECONDS": "2"}, 4.5)
        if outputs_in_rc() != want:
            failures.append(f"rc.xml not put back: {outputs_in_rc()}")
        now = positions()
        if now != {"HEADLESS-1": (0, 0), "HEADLESS-2": (0, 720)}:
            failures.append(f"Waybox's layout not put back: {now}")
        log.flush()
        reloads = open(log_path).read().count("Reconfigured")
        if reloads < 3:
            failures.append(f"Waybox reloaded {reloads} times (save, save, revert)")
    except (RuntimeError, OSError, ValueError, KeyError) as e:
        failures.append(str(e))
    finally:
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
        print(open(rc_path).read(), file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf monitors: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
