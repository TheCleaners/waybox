#!/usr/bin/env python3
"""Integration test: a color field changed many times in a row (dragging in
GTK's color editor) keeps wbconf responsive.

Each change tints the field. Tinting it through one stylesheet for the
whole display restyled every widget of wbconf on each change: a color
picked froze it for minutes. Here the bell color's button is changed 200
times (WBCONF_TEST_TOC set:colorburst...), which must take well under a
second or two, and leave the last color in the field and its tint.

Skips (exit 77) without a headless display.

Usage: wbconf_color_burst_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import re
import select
import shutil
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
    workdir = tempfile.mkdtemp(prefix="waybox-colorburst-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID"):
        env.pop(k, None)
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
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                GSETTINGS_BACKEND="memory", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_TOC="set:colorburst.waybox.bell.color=200")
    failures = []
    try:
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        # Read until it says how it went (or 30 s: stuck), then end it.
        err = ""
        deadline = time.time() + 30
        while time.time() < deadline and "colortint:" not in err:
            ready, _, _ = select.select([app.stderr], [], [], 0.5)
            if ready:
                line = app.stderr.readline()
                if not line:
                    break
                err += line
        app.kill()
        app.wait()
        burst = re.search(r"colorburst:(\d+):(\d+)", err)
        tint = re.search(r"colortint:([^|]*)\|(.*)", err)
        if burst is None:
            failures.append("the burst didn't finish (wbconf stuck restyling?)")
        elif int(burst.group(2)) > 3000:
            failures.append(f"200 color changes took {burst.group(2)} ms")
        else:
            print(f"200 color changes: {burst.group(2)} ms")
        # The last one: (199, 128, 113).
        if tint is None or tint.group(1).lower() != "#c78071" or \
                "rgba(199, 128, 113" not in tint.group(2):
            failures.append(f"the field or its tint isn't the last color: "
                            f"{tint.groups() if tint else None}")
    finally:
        proc.terminate()
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
    print("integration wbconf_color_burst: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
