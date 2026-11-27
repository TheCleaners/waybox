#!/usr/bin/env python3
"""Integration test: wbconf's location picker (wlsunset's Location=).

wlsunset's integration names its latitude and longitude options a place
(Location=latitude,longitude): wbconf shows a map above them. From its
command (-l 51.5 -L -0.1): London. A city found ("lisbon"): its place in
both fields. A click at the map's middle: 0, 0. Typed with a comma (as in
locales that write decimals so): read. The time zone's city (TZ set to
Europe/Lisbon): tzdata's place for it (skipped without zone1970.tab).
The crosshair: with no location portal, or one without GeoClue behind
it, nothing to ask (the button off); with one (a
stand-in on a private bus), asked at neighborhood accuracy, the place it
gives taken (Paris); refused, the place left as it was and why said.
(Skipped without dbus-daemon or Python's GLib bindings.)
The map: zoomed and moved (the buttons, the wheel around the pointer, a
drag), within their limits.
Turned on, its settings unfold, the first time only.
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
                                          "geo:wlsunset=click:0.5,0.5",
                                          "geo:wlsunset=type:38,72",
                                          "geo:wlsunset=find:lisbon",
                                          "geo:wlsunset=click:0.75,0.5",
                                          "geo:wlsunset=zoom:-1",
                                          "geo:wlsunset=pan:0.5,0",
                                          "geo:wlsunset=zoom:1@0.75,0.5",
                                          "geo:wlsunset=zoom:-9",
                                          "geo:wlsunset=zone",
                                          # Turned on: its settings unfold, the first time only.
                                          "pagefold:wlsunset",
                                          "set:program.wlsunset=on", "pagefold:wlsunset",
                                          "set:program.wlsunset=off", "pages-folded",
                                          "set:program.wlsunset=on", "pagefold:wlsunset"]))
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * 19 + 2)
    app.kill()
    out = app.communicate()[1].splitlines()
    lines = [l.split("|") for l in out if l.startswith("geo|")]
    folds = [l.split("|")[1] for l in out if l.startswith("pagefold:wlsunset|")]

    failures = []
    # Lisbon as the cities file has it.
    lisbon = next(l.split("\t") for l in open(os.path.join(root, "data", "geo", "cities.tsv"),
                                             encoding="utf-8")
                  if l.startswith("Lisbon\t"))
    lat, lon = lisbon[2], lisbon[3]
    east = f"{float(lon) + 360 / 8 / 4:.2f}"  # a quarter of the view (45 degrees) east
    # (lat field, lon field, caption start, view: zoom, its middle)
    want = [
        ("51.5", "-0.1", "51.50, -0.10 \u00b7 near London", "1.00 0.00 0.00"),
        ("0.00", "0.00", "0.00, 0.00 \u00b7 near ", "1.00 0.00 0.00"),
        ("38,72", "0.00", "38.72, 0.00 \u00b7 near ", "1.00 0.00 0.00"),
        # Found: shown near (zoom 8) in the middle.
        (lat, lon, f"{lat}, {lon} \u00b7 near Lisbon, Portugal", f"8.00 {lat} {lon}"),
        # A click a quarter right of the middle: that far east at zoom 8.
        (lat, east, f"{lat}, {east}", f"8.00 {lat} {lon}"),
        # The minus button: around the middle.
        (lat, east, "", f"4.00 {lat} {lon}"),
        # Dragged half the view right: the middle 45 degrees west.
        (lat, east, "", f"4.00 {lat} {float(lon) - 45:.2f}"),
        # The wheel a quarter right: that place kept under it.
        (lat, east, "", f"8.00 {lat} {float(lon) - 45 + 22.5 - 11.25:.2f}"),
        # Out past the whole world: the whole world, no further.
        (lat, east, "", "1.00 0.00 0.00"),
    ]
    if len(lines) < 10:
        failures.append(f"geo steps: {lines}")
    else:
        for i, (got, (la, lo, caption, view)) in enumerate(zip(lines, want)):
            if got[2] != la or got[3] != lo or not got[4].startswith(caption) or got[5] != view:
                failures.append(f"geo step {i}: {got[2:]} not {la}, {lo}, {caption}..., {view}")
        zone = "/usr/share/zoneinfo/zone1970.tab"
        if os.path.exists(zone):
            # zone1970.tab's Lisbon: +3843-00908.
            if lines[9][2:4] != ["38.72", "-9.13"] or not lines[9][5].startswith("8.00 "):
                failures.append(f"the time zone's city: {lines[9][2:]}")
    if folds != ["folded", "open", "folded"]:
        failures.append(f"settings on first turning on (folded, open; then as kept): {folds}")
    # The crosshair, with no portal (the session bus off): nothing to ask.
    no = [l for l in lines if l[0] == "geo"]
    # (The steps above ran without one; the button's answer is checked below.)
    have_gi = subprocess.run([sys.executable, "-c", "import gi; gi.require_version('Gio','2.0');"
                              "from gi.repository import Gio"], capture_output=True).returncode == 0
    if shutil.which("dbus-daemon") is not None and have_gi:
        here = os.path.dirname(os.path.abspath(__file__))
        for mode, want_fields, want_caption in (
                ("allow", ["48.86", "2.35"], "48.86, 2.35 \u00b7 near Paris"),
                ("deny", ["51.5", "-0.1"], "The system didn't say where this is"),
                ("no-geoclue", ["51.5", "-0.1"], "")):
            daemon = subprocess.run(["dbus-daemon", "--session", "--fork", "--print-address=1",
                                     "--print-pid=1"], capture_output=True, text=True)
            address, pid = daemon.stdout.split("\n")[:2]
            try:
                fake = subprocess.Popen([sys.executable, os.path.join(here, "fake_location_portal.py")]
                                        + (["deny"] if mode == "deny" else []),
                                        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address),
                                        stdout=subprocess.PIPE, text=True)
                wbtest.reap_on_exit(fake)
                fake.stdout.readline()  # ready
                app = subprocess.Popen([wbconf], env=dict(
                    wenv, DBUS_SESSION_BUS_ADDRESS=address, WBCONF_GEOCLUE="0" if mode == "no-geoclue" else "1",
                    WBCONF_TEST_TOC="dump;geo:wlsunset=locate;geo:wlsunset=show"),
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                wbtest.reap_on_exit(app)
                time.sleep(0.8 * 3 + 2)
                app.kill()
                err = app.communicate()[1].splitlines()
                fake.kill()
                said = fake.communicate()[0]
            finally:
                subprocess.run(["kill", pid], capture_output=True)
            asked = [l for l in err if l.startswith("geo-locate:")]
            got = [l.split("|") for l in err if l.startswith("geo|")]
            if mode == "no-geoclue":
                # The portal there, GeoClue not: nothing asked, nothing moved.
                if asked != ["geo-locate:no portal"] or "create" in said:
                    failures.append(f"no GeoClue: {asked} {said!r}")
                elif not got or got[-1][2:4] != want_fields:
                    failures.append(f"no GeoClue: moved to {got[-1:]}")
                continue
            if asked != ["geo-locate:asked"]:
                failures.append(f"{mode}: the crosshair didn't ask: {asked}")
            elif "create accuracy=3" not in said:
                failures.append(f"{mode}: not asked at neighborhood accuracy: {said!r}")
            elif not got or got[-1][2:4] != want_fields or not got[-1][4].startswith(want_caption):
                failures.append(f"{mode}: after asking: {got[-1:] }")

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
