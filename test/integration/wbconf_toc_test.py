#!/usr/bin/env python3
"""Integration test: wbconf's sidebar, a tree of categories and sections.

Driven and described step by step (WBCONF_TEST_TOC, a dump on stderr:
">Name" a closed category, "vName" an open one, ".Name" a section, "*" the
one selected):
1. At the start: the first category open and selected, its sections
   under it, the others closed.
2. A section clicked (Edge Tiling, under Windows): its category opens,
   the section is selected; the first one, opened for being read, closes.
3. A category clicked twice: open, then closed.
4. A search: only the categories (and sections) with a match, open.
5. The search cleared, scrolled to the end: the last category open and
   its last section selected.
Skips (exit 77) without a headless display.

Usage: wbconf_toc_test.py <waybox> <wbconf> <schema-dir>
"""
import os
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
    workdir = tempfile.mkdtemp(prefix="waybox-wbtoc-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
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
    steps = ["dump", "click:Edge Tiling", "dump", "click:Monitors", "dump", "click:Monitors",
             "dump", "search:bell", "dump", "search:", "scroll:1", "dump"]
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid), WBCONF_TEST_TOC=";".join(steps))
    failures = []
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * (len(steps) + 2) + 1.5)
    app.kill()
    _, err = app.communicate()
    dumps = [l.split("|")[1:] for l in err.splitlines() if l.startswith("toc-dump:")]
    if len(dumps) != 6:
        failures.append(f"{len(dumps)} dumps, not 6: {err[-800:]}")
    else:
        def cats(d):
            return [x for x in d if x[0] in "v>"]

        def opened(d):
            return [x[1:].rstrip("*") for x in d if x[0] == "v"]

        def selected(d):
            return [x[1:].rstrip("*") for x in d if x.endswith("*")]

        start, wall, mon_open, mon_closed, search, end = dumps
        if opened(start) != ["Appearance"] or selected(start) != ["Appearance"] or \
                ".Fonts" not in start or len(cats(start)) < 8:
            failures.append(f"at the start: {start}")
        if opened(wall) != ["Windows"] or selected(wall) != ["Edge Tiling"] or \
                ".Fonts" in wall:
            failures.append(f"Edge Tiling clicked: {wall}")
        if "Monitors" not in opened(mon_open) or selected(mon_open) != ["Monitors"]:
            failures.append(f"Monitors clicked: {mon_open}")
        if "Monitors" in opened(mon_closed):
            failures.append(f"Monitors clicked again: {mon_closed}")
        if len(cats(search)) >= len(cats(start)) or not opened(search) or \
                set(opened(search)) != {c[1:].rstrip("*") for c in cats(search)}:
            failures.append(f"searching bell: {search}")
        last = [c[1:].rstrip("*") for c in cats(end)][-1] if cats(end) else None
        if last is None or last not in opened(end) or not selected(end):
            failures.append(f"scrolled to the end: {end}")
        elif "Windows" in opened(end):
            failures.append(f"a category opened for being read stayed open: {end}")
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
    print("integration wbconf toc: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
