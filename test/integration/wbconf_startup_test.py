#!/usr/bin/env python3
"""Integration test: wbconf switches programs started from elsewhere.

The user's autostart script has a program start line, a commented-out
example and an if block; XDG autostart has an entry of the user's and a
system one. Switched as in the window (WBCONF_TEST_SET) and saved with
Ctrl+S:
- the script's start line is commented out and the example back in,
  nothing else changes, it stays executable, and a backup is kept;
- the system entry is switched off by a Hidden=true copy in the user's
  autostart folder (the system file untouched), the user's entry in place.

Skips (exit 77) without wtype, or a headless display.

Usage: wbconf_startup_test.py <waybox> <wbconf> <schema-dir>
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

SCRIPT = """#!/bin/sh
mako &
# my notes
#swayidle -w timeout 600 'wlopm --off "*"' &
if true; then
    swaybg &
fi
"""
EXPECTED = """#!/bin/sh
#mako &
# my notes
swayidle -w timeout 600 'wlopm --off "*"' &
if true; then
    swaybg &
fi
"""
SYSTEM_BAR = "[Desktop Entry]\nType=Application\nName=Bar\nExec=bar --tray\n"
USER_FOO = "[Desktop Entry]\nType=Application\nName=Foo\nExec=foo\n"


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbstartup-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(cfg, "autostart"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    script_path = os.path.join(cfg, "waybox", "autostart")
    with open(script_path, "w") as f:
        f.write(SCRIPT)
    os.chmod(script_path, 0o755)
    with open(os.path.join(cfg, "autostart", "foo.desktop"), "w") as f:
        f.write(USER_FOO)
    sysxdg = os.path.join(workdir, "sysxdg")
    os.makedirs(os.path.join(sysxdg, "autostart"))
    with open(os.path.join(sysxdg, "autostart", "bar.desktop"), "w") as f:
        f.write(SYSTEM_BAR)
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
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                XDG_CONFIG_DIRS=sysxdg,
                WBCONF_TEST_SET="script.2=off;script.4=on;"
                                "xdg.bar.desktop=off;xdg.foo.desktop=off")
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
        out = open(script_path).read()
        if out != EXPECTED:
            failures.append(f"the script isn't as expected:\n{out}")
        if not os.access(script_path, os.X_OK):
            failures.append("the script isn't executable any more")
        if not os.path.exists(script_path + ".bak") or open(script_path + ".bak").read() != SCRIPT:
            failures.append("no backup of the previous script")
        bar_user = os.path.join(cfg, "autostart", "bar.desktop")
        if not os.path.exists(bar_user):
            failures.append("the system entry wasn't switched off in the user's folder")
        else:
            text = open(bar_user).read()
            if "Hidden=true" not in text or "Exec=bar --tray" not in text:
                failures.append(f"the user's copy of the system entry is wrong:\n{text}")
        if open(os.path.join(sysxdg, "autostart", "bar.desktop")).read() != SYSTEM_BAR:
            failures.append("the system's autostart file was changed")
        foo = open(os.path.join(cfg, "autostart", "foo.desktop")).read()
        if "Hidden=true" not in foo:
            failures.append(f"the user's entry wasn't switched off:\n{foo}")
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
    print("integration wbconf startup: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
