#!/usr/bin/env python3
"""Integration test: wbconf's typed integration options (Type=).

Two made-up programs with integration files of every type, in rc.xml's
<startup>. Their options are set through their widgets (WBCONF_TEST_SET
option.ID.OPTION=VALUE) and saved with Ctrl+S:
- fakebg: an image path, a choice from the drop-down, a colour from the
  colour button (written as #rrggbb), a switch turned off (its flag goes),
  a number: the command becomes exactly what's expected;
- fakefg: a choice the command has that isn't one of the choices is kept
  as it was, while a switch is turned on (its flag added alone).
Skips (exit 77) without wtype, or a headless display.

Usage: wbconf_options_test.py <waybox> <wbconf> <schema-dir>
"""
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

INTEGRATION = """[Integration]
Name={name}
Category=Wallpaper
Comment=A made-up program
Exec={name}

[Option image]
Label=Image
Flag=-i
Type=image

[Option mode]
Label=Mode
Flag=-m
Type=choice
Choices=a,b,c

[Option colour]
Label=Colour
Flag=-c
Type=color

[Option quiet]
Label=Quiet
Flag=-q
Type=switch

[Option every]
Label=Every
Flag=-e
Type=number
Min=1
Max=10
"""

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <startup>
      <program name="fakebg" enabled="no">fakebg -m b -q</program>
      <program name="fakefg" enabled="no">fakefg -m weird</program>
    </startup>
  </waybox>
</openbox_config>
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wboptions-", dir="/tmp")
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    integ = os.path.join(workdir, "share", "waybox", "integrations")
    os.makedirs(integ)
    for name in ("fakebg", "fakefg"):
        with open(os.path.join(bindir, name), "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(os.path.join(bindir, name), 0o755)
        with open(os.path.join(integ, name + ".integration"), "w") as f:
            f.write(INTEGRATION.format(name=name))
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC)
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
                GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_SET="option.fakebg.image=~/pictures/sea.png;"
                                "option.fakebg.mode=c;"
                                "option.fakebg.colour=rgb(255,0,0);"
                                "option.fakebg.quiet=;"
                                "option.fakebg.every=5;"
                                "option.fakefg.quiet=on",
                XDG_DATA_DIRS=os.path.join(workdir, "share"),
                PATH=bindir + ":" + os.environ.get("PATH", ""))
    failures = []
    app = None
    try:
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        time.sleep(4)
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(1.5)
        out = open(rc_path).read()
        bg = re.search(r'<program name="fakebg"[^>]*>([^<]*)</program>', out)
        fg = re.search(r'<program name="fakefg"[^>]*>([^<]*)</program>', out)
        want_bg = "fakebg -m c -i ~/pictures/sea.png -c '#ff0000' -e 5"
        if bg is None or bg.group(1).replace("&apos;", "'") != want_bg:
            failures.append(f"fakebg's command: {bg.group(1) if bg else None!r}, "
                            f"not {want_bg!r}")
        want_fg = "fakefg -m weird -q"
        if fg is None or fg.group(1) != want_fg:
            failures.append(f"fakefg's command: {fg.group(1) if fg else None!r}, "
                            f"not {want_fg!r}")
    except (RuntimeError, OSError) as e:
        failures.append(str(e))
    finally:
        if app is not None and app.poll() is None:
            app.kill()
        time.sleep(0.3)
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
    print("integration wbconf options: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
