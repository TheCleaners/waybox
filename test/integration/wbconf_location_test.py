#!/usr/bin/env python3
"""Integration test: wbconf's location page (Page=location, the location
agent's integration): the answers kept always, from the permission store.

A private session bus with the real xdg-permission-store, two answers in
its location table: Maps allowed to within a city, three days ago; a tool
denied today. The page lists both, in words; Forget on the tool: it goes
from the store, and the list follows; Forget All: none left, and the
note says what's kept here. Without a session bus: the page says the
store isn't there.

Skips (exit 77) without dbus-daemon, xdg-permission-store, the Python
GObject bindings, or a headless display.

Usage: wbconf_location_test.py <waybox> <wbconf> <schema-dir> <source-root>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77
STORE = "/usr/libexec/xdg-permission-store"


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, root = sys.argv[1:]
    try:
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib
    except (ImportError, ValueError):
        print("SKIP: no Python GObject bindings", file=sys.stderr)
        sys.exit(SKIP)
    if shutil.which("dbus-daemon") is None or not os.access(STORE, os.X_OK):
        print("SKIP: dbus-daemon or xdg-permission-store missing", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-locpage-", dir="/tmp")
    procs = []
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"><waybox><startup>'
                '<program name="waybox-location-agent">waybox-location-agent</program>'
                '</startup></waybox></openbox_config>\n')
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    with open(os.path.join(bindir, "waybox-location-agent"), "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(os.path.join(bindir, "waybox-location-agent"), 0o755)
    share = os.path.join(workdir, "share")
    integ = os.path.join(share, "waybox", "integrations")
    os.makedirs(integ)
    shutil.copy(os.path.join(root, "data", "integrations", "waybox-location-agent.integration"),
                integ)
    os.makedirs(os.path.join(share, "applications"))
    with open(os.path.join(share, "applications", "org.example.Maps.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Maps\nExec=true\n")

    bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address"],
                           stdout=subprocess.PIPE, text=True)
    procs.append(bus)
    address = bus.stdout.readline().strip()
    data = os.path.join(workdir, "data")
    os.makedirs(data)
    benv = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address, XDG_DATA_HOME=data)
    procs.append(subprocess.Popen([STORE], env=benv, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL))
    conn = Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def store_call(method, args, reply=None):
        return conn.call_sync("org.freedesktop.impl.portal.PermissionStore",
                              "/org/freedesktop/impl/portal/PermissionStore",
                              "org.freedesktop.impl.portal.PermissionStore", method, args,
                              reply, Gio.DBusCallFlags.NONE, 5000, None)

    now = int(time.time())
    for _ in range(50):  # the store, once it's up (activation isn't set up here)
        try:
            store_call("SetPermission", GLib.Variant("(sbssas)", (
                "location", True, "location", "org.example.Maps", ["CITY", str(now - 3 * 86400)])))
            break
        except GLib.Error:
            time.sleep(0.1)
    store_call("SetPermission", GLib.Variant("(sbssas)", (
        "location", True, "location", "some.tool", ["NONE", str(now)])))

    def kept():
        try:
            r = store_call("Lookup", GLib.Variant("(ss)", ("location", "location")))
            return sorted(dict(r.unpack()[0]))
        except GLib.Error:
            return []

    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0", PATH=bindir + ":" + os.environ.get("PATH", ""))
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    procs.append(proc)
    sock = None
    for _ in range(50):
        time.sleep(0.1)
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    failures = []
    try:
        if sock is None:
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            return SKIP
        wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                    DBUS_SESSION_BUS_ADDRESS=address, GSETTINGS_BACKEND="memory",
                    GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                    XDG_DATA_DIRS=share, WAYBOX_PID=str(proc.pid),
                    WBCONF_TEST_TOC=";".join(["location:dump", "location:forget:some.tool",
                                              "location:dump", "location:forget-all",
                                              "location:dump"]))

        def run_wbconf(e, steps):
            app = subprocess.Popen([wbconf], env=e, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
            wbtest.reap_on_exit(app)
            time.sleep(0.8 * steps + 3)
            app.kill()
            err = app.communicate()[1]
            with open(os.path.join(workdir, "wbconf.log"), "a") as f:
                f.write(err)
            return [l[len("location-dump:"):] for l in err.splitlines()
                    if l.startswith("location-dump:")]

        before = kept()
        dumps = run_wbconf(wenv, 5)
        if before != ["org.example.Maps", "some.tool"]:
            failures.append(f"the store wasn't seeded: {before}")
        maps = "org.example.Maps=Allowed, to within a city · answered 3 days ago"
        tool = "some.tool=Denied · answered today"
        if len(dumps) != 3:
            failures.append(f"page dumps: {dumps}")
        else:
            if not dumps[0].startswith(f"|{maps}|{tool}|note=Given at once"):
                failures.append(f"listed: {dumps[0]}")
            if not dumps[1].startswith(f"|{maps}|note="):
                failures.append(f"after forgetting the tool: {dumps[1]}")
            if not dumps[2].startswith("|note=None yet."):
                failures.append(f"after Forget All: {dumps[2]}")
        if kept():
            failures.append(f"Forget All left the store with {kept()}")
        # No session bus: the page says the store isn't there.
        dumps = run_wbconf(dict(wenv, DBUS_SESSION_BUS_ADDRESS="disabled:",
                                WBCONF_TEST_TOC="location:dump"), 1)
        if not dumps or not dumps[0].startswith("unreachable|note=The permission store"):
            failures.append(f"with no session bus: {dumps}")
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.kill()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        log.close()
    if failures:
        with open(os.path.join(workdir, "wbconf.log")) as f:
            print(f.read()[-3000:], file=sys.stderr)
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf_location: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
