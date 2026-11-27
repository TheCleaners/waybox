#!/usr/bin/env python3
"""Integration test: wbconf's location picker (wlsunset's Location=).

wlsunset's integration names its latitude and longitude options a place
(Location=latitude,longitude): wbconf shows a map above them. From its
command (-l 51.5 -L -0.1): London. A city found ("lisbon"): its place in
both fields. A click at the map's middle: 0, 0. Typed with a comma (as in
locales that write decimals so): read. The time zone's city (TZ set to
Europe/Lisbon): tzdata's place for it (skipped without zone1970.tab).
Skips (exit 77) without a headless display.

Usage: wbconf_geo_test.py <waybox> <wbconf> <schema-dir> <source-root>
"""
import os
import shutil
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
    waybox, wbconf, schema, root = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-geo-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"></openbox_config>\n')
    # wlsunset, as installed (a stand-in: never run).
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    with open(os.path.join(bindir, "wlsunset"), "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(os.path.join(bindir, "wlsunset"), 0o755)
    integ = os.path.join(workdir, "share", "waybox", "integrations")
    os.makedirs(integ)
    shutil.copy(os.path.join(root, "data", "integrations", "wlsunset.integration"), integ)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
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
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WBCONF_GEO_DIR=os.path.join(root, "data", "geo"), TZ="Europe/Lisbon",
                XDG_DATA_DIRS=os.path.join(workdir, "share"),
                PATH=bindir + ":" + os.environ.get("PATH", ""),
                WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_TOC=";".join(["dump", "geo:wlsunset=show",
                                          "geo:wlsunset=find:lisbon",
                                          "geo:wlsunset=click:0.5,0.5",
                                          "geo:wlsunset=type:38,72",
                                          "geo:wlsunset=zone"]))
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * 6 + 2)
    app.kill()
    lines = [l.split("|") for l in app.communicate()[1].splitlines() if l.startswith("geo|")]

    failures = []
    # Lisbon as the cities file has it.
    lisbon = next(l.split("\t") for l in open(os.path.join(root, "data", "geo", "cities.tsv"),
                                             encoding="utf-8")
                  if l.startswith("Lisbon\t"))
    want = [
        ("51.5", "-0.1", "51.50, -0.10 · near London"),
        (lisbon[2], lisbon[3], f"{lisbon[2]}, {lisbon[3]} · near Lisbon, Portugal"),
        ("0.00", "0.00", "0.00, 0.00 · near "),
        ("38,72", "0.00", "38.72, 0.00 · near "),
    ]
    if len(lines) < 5:
        failures.append(f"geo steps: {lines}")
    else:
        for got, (lat, lon, caption) in zip(lines, want):
            if got[2] != lat or got[3] != lon or not got[4].startswith(caption):
                failures.append(f"geo: {got[2:]} not {lat}, {lon}, {caption}...")
        zone = "/usr/share/zoneinfo/zone1970.tab"
        if os.path.exists(zone):
            # zone1970.tab's Lisbon: +3843-00908.
            if lines[4][2:4] != ["38.72", "-9.13"]:
                failures.append(f"the time zone's city: {lines[4][2:]}")
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
    print("integration wbconf geo: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
