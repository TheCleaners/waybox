#!/usr/bin/env python3
"""Integration test: a program XDG autostart starts, turned on in wbconf.

The system's autostart folder (XDG_CONFIG_DIRS) has wbt-applet.desktop and
wbt-other.desktop; a wbt-applet runs, as that autostart started it.
1. wbt-applet's row says where it's started from ("the system's autostart
   (wbt-applet.desktop)").
2. Turned on, asked, "Let Waybox Start It" (WBCONF_TEST_XDG=take): no
   warning; saved: a Hidden=true copy in ~/.config/autostart, the system's
   file as it was, rc.xml's <startup> has it, the running copy stopped.
3. wbt-other turned on, "Cancel": switched back off, nothing written.
Skips (77) without ps or a headless display.

Usage: wbconf_xdg_move_test.py <waybox> <wbconf> <schema-dir> <fake_daemon>
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import wbtest

SKIP = 77
INTEGRATION = "[Integration]\nName={name}\nCategory=Other\nComment=test\nExec={name}\n"
DESKTOP = "[Desktop Entry]\nType=Application\nName={name}\nExec={name}\n"


def pids_named(name):
    out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "comm="], capture_output=True,
                         text=True).stdout
    return {int(p) for p, c in (l.split(None, 1) for l in out.splitlines() if l.strip())
            if c.strip() == name}


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, fake = sys.argv[1:]
    if shutil.which("ps") is None:
        print("SKIP: ps not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-xdgmove-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    sysxdg = os.path.join(workdir, "etc-xdg")
    bindir = os.path.join(workdir, "bin")
    for d in (os.path.join(cfg, "waybox", "integrations"), os.path.join(sysxdg, "autostart"),
              bindir):
        os.makedirs(d)
    for name in ("wbt-applet", "wbt-other"):
        shutil.copy(fake, os.path.join(bindir, name))
        with open(os.path.join(cfg, "waybox", "integrations", f"{name}.integration"), "w") as f:
            f.write(INTEGRATION.format(name=name))
        with open(os.path.join(sysxdg, "autostart", f"{name}.desktop"), "w") as f:
            f.write(DESKTOP.format(name=name))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_CONFIG_DIRS=sysxdg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0",
               PATH=bindir + ":" + os.environ.get("PATH", "/usr/bin:/bin"))
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    # The copy "XDG autostart started" (made-up name: nothing else matches).
    theirs = pids_named("wbt-applet")
    applet = subprocess.Popen([os.path.join(bindir, "wbt-applet")], env=env,
                              start_new_session=True, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    wbtest.reap_on_exit(applet)
    time.sleep(0.5)
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
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid))
    failures = []

    def run(steps, answer):
        e = dict(cenv, WBCONF_TEST_TOC=";".join(steps), WBCONF_TEST_XDG=answer)
        a = subprocess.Popen([wbconf], env=e, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(a)
        time.sleep(0.8 * (len(steps) + 2) + 2)
        a.kill()
        _, err = a.communicate()
        return [l for l in err.splitlines() if l.startswith(("progstatus:", "xdg-asked:"))]

    system_file = os.path.join(sysxdg, "autostart", "wbt-applet.desktop")
    system_before = open(system_file).read()
    out = run(["progstatus:wbt-applet", "set:program.wbt-applet=on", "progstatus:wbt-applet",
               "save"], "take")
    status = [l for l in out if l.startswith("progstatus:")]
    if not status or "started by the system's autostart (wbt-applet.desktop)" not in status[0]:
        failures.append(f"where it's started from: {status[:1]}")
    if "xdg-asked:wbt-applet|wbt-applet.desktop" not in out:
        failures.append(f"not asked: {out}")
    if len(status) < 2 or "would run twice" in status[1] or status[1].endswith("|warning"):
        failures.append(f"after letting Waybox start it: {status[1:]}")
    copy = os.path.join(cfg, "autostart", "wbt-applet.desktop")
    if not os.path.exists(copy) or "Hidden=true" not in open(copy).read():
        failures.append("no Hidden=true copy in ~/.config/autostart")
    if open(system_file).read() != system_before:
        failures.append("the system's file was changed")
    if not re.search(r"<program[^>]*>\s*(<command>)?wbt-applet", open(rc_path).read()):
        failures.append(f"rc.xml has no wbt-applet:\n{open(rc_path).read()}")
    time.sleep(0.5)
    if applet.poll() is None:
        failures.append("the running copy wasn't stopped")
    # 3. Cancelled: back off, nothing written.
    out = run(["set:program.wbt-other=on", "progstatus:wbt-other", "save"], "cancel")
    status = [l for l in out if l.startswith("progstatus:wbt-other")]
    if not status or not status[0].startswith("progstatus:wbt-other|off"):
        failures.append(f"cancelled, not switched back off: {status}")
    if os.path.exists(os.path.join(cfg, "autostart", "wbt-other.desktop")):
        failures.append("cancelled, yet a copy was written")
    for pid in pids_named("wbt-applet") - theirs - {applet.pid}:
        try:
            os.kill(pid, 15)  # Waybox's own copy, started by the reload
        except OSError:
            pass
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
    print("integration wbconf xdg move: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
