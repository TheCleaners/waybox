#!/usr/bin/env python3
"""Integration test: wbconf reading rc.xml again (Reload, and after Save).

Two made-up Openbox themes installed, one chosen in rc.xml: the theme list
is rebuilt on each reload. Removing its selected row once redrew the
thumbnails of rows already gone (a GTK critical per theme, every save).
Here wbconf runs with G_DEBUG=fatal-criticals: any critical aborts it.
Skips (exit 77) without a headless display.

Usage: wbconf_reload_test.py <waybox> <wbconf> <schema-dir>
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

THEMERC = """window.active.title.bg: flat solid
window.active.title.bg.color: {color}
window.active.label.text.color: #ffffff
window.inactive.title.bg: flat solid
window.inactive.title.bg.color: #888888
menu.items.bg: flat solid
menu.items.bg.color: #eeeeee
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbreload-", dir="/tmp")
    share = os.path.join(workdir, "share")
    for name, color in (("TestOne", "#336699"), ("TestTwo", "#993366")):
        d = os.path.join(share, "themes", name, "openbox-3")
        os.makedirs(d)
        with open(os.path.join(d, "themerc"), "w") as f:
            f.write(THEMERC.format(color=color))
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc">'
                '<theme><name>TestTwo</name></theme></openbox_config>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, XDG_DATA_DIRS=share,
               XDG_DATA_HOME=os.path.join(workdir, "data"), WB_RC_XML="", WB_MENU_XML="",
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
                WAYBOX_PID=str(proc.pid), G_DEBUG="fatal-criticals",
                WBCONF_TEST_SET="reload=1;reload=1")
    failures = []
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(4)
    if app.poll() is not None:
        failures.append(f"wbconf ended ({app.returncode}): "
                        f"{[l for l in app.stderr.read().splitlines() if 'CRITICAL' in l][:2]}")
    else:
        app.kill()
    app.wait()
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
    print("integration wbconf reload: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
