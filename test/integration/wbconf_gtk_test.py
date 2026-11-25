#!/usr/bin/env python3
"""Integration test: wbconf's GTK appearance, and broken icon fallbacks.

An icon theme falls back to one that isn't installed. In wbconf (GSettings
in memory: the user's never touched) the theme is chosen, its gaps are
filled, and Dark is chosen, then saved with Ctrl+S:
- a stand-in for the missing theme is made in the user's icons folder,
  falling back to the complete theme, hidden and marked as wbconf's;
- the GTK 4 settings.ini gets the icon theme and the dark preference,
  and keeps its other settings.

Skips (exit 77) without wtype, or a headless display.

Usage: wbconf_gtk_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def theme(base, tid, text):
    os.makedirs(os.path.join(base, tid), exist_ok=True)
    with open(os.path.join(base, tid, "index.theme"), "w") as f:
        f.write(text)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbgtk-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(cfg, "gtk-4.0"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    ini = os.path.join(cfg, "gtk-4.0", "settings.ini")
    with open(ini, "w") as f:
        f.write("[Settings]\ngtk-icon-theme-name=breeze\ngtk-enable-animations=true\n")
    datahome = os.path.join(workdir, "data")
    fake = os.path.join(workdir, "share")
    icons = os.path.join(fake, "icons")
    theme(icons, "TestIcons", "[Icon Theme]\nName=TestIcons\nInherits=TestGone,hicolor\n"
                              "Directories=16x16/actions\n")
    theme(icons, "Adwaita", "[Icon Theme]\nName=Adwaita\nInherits=hicolor\n"
                            "Directories=16x16/actions\n")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                GSETTINGS_BACKEND="memory",            # never the user's settings
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                XDG_DATA_HOME=datahome,
                XDG_DATA_DIRS=fake + ":" + os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share"),
                WBCONF_TEST_SET="gtk.icons=TestIcons;gtk.fill=1;gtk.scheme=prefer-dark")
    failures = []
    app = None
    try:
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        time.sleep(4)
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(1.5)
        stand_in = os.path.join(datahome, "icons", "TestGone", "index.theme")
        if not os.path.exists(stand_in):
            failures.append("no stand-in for the missing fallback")
        else:
            text = open(stand_in).read()
            for want in ("Inherits=Adwaita,hicolor", "Hidden=true", "X-Waybox-Stand-In=true"):
                if want not in text:
                    failures.append(f"the stand-in lacks {want!r}:\n{text}")
        out = open(ini).read()
        for want in ("gtk-icon-theme-name=TestIcons", "gtk-application-prefer-dark-theme=true",
                     "gtk-enable-animations=true"):
            if want not in out:
                failures.append(f"settings.ini lacks {want!r}:\n{out}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if app is not None and app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
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
    print("integration wbconf gtk: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
