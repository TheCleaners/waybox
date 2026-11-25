#!/usr/bin/env python3
"""Integration test: wbconf shows integrated programs' versions.

Integration files with Version= commands (stand-ins for real programs):
- one prints "fakebar v3.14.1": its version is 3.14.1;
- one never answers: stopped after three seconds, no version;
- its command runs without the session's display or bus: it can't reach
  the Wayland display or D-Bus (checked from what it saw);
- a program with no Version= is never run to ask.
Skips (77) without a headless display.

Usage: wbconf_versions_test.py <waybox> <wbconf> <schema-dir>
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


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbver-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    bindir = os.path.join(workdir, "bin")
    integ = os.path.join(cfg, "waybox", "integrations")
    for d in (integ, bindir):
        os.makedirs(d)
    seen = os.path.join(workdir, "seen-env")
    asked_none = os.path.join(workdir, "asked-none")
    hang_pid = os.path.join(workdir, "hang-pid")
    programs = {
        # Prints its version, and what it could reach.
        "wbt-bar": f'#!/bin/sh\necho "WAYLAND=$WAYLAND_DISPLAY DISPLAY=$DISPLAY '
                   f'BUS=$DBUS_SESSION_BUS_ADDRESS" > {seen}\necho "fakebar v3.14.1"\n',
        # Never answers.
        "wbt-hang": f"#!/bin/sh\necho $$ > {hang_pid}\nexec sleep 100\n",
        # No Version=: must never run.
        "wbt-none": f"#!/bin/sh\ntouch {asked_none}\n",
    }
    for name, text in programs.items():
        path = os.path.join(bindir, name)
        with open(path, "w") as f:
            f.write(text)
        os.chmod(path, 0o755)
    for name, version in (("wbt-bar", "wbt-bar --version"), ("wbt-hang", "wbt-hang -v"),
                          ("wbt-none", None)):
        with open(os.path.join(integ, f"{name}.integration"), "w") as f:
            f.write(f"[Integration]\nName={name}\nCategory=Other\nComment=test\nExec={name}\n"
                    + (f"Version={version}\n" if version else ""))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               PATH=bindir + ":" + os.environ.get("PATH", "/usr/bin:/bin"),
               ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    failures = []
    wb_log = os.path.join(workdir, "wbconf.log")
    try:
        cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                    DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent/test-bus",
                    GSETTINGS_BACKEND="memory", WBCONF_SCHEMA_DIR=schema,
                    G_MESSAGES_DEBUG="wbconf")
        with open(wb_log, "w") as out:
            app = subprocess.Popen([wbconf], env=cenv, stdout=out, stderr=subprocess.STDOUT)
            wbtest.reap_on_exit(app)
            time.sleep(6)  # past the three seconds the hanging one gets
            app.terminate()
            app.wait(timeout=5)
        text = open(wb_log).read()
        if 'version of wbt-bar: "3.14.1"' not in text:
            failures.append("wbt-bar's version wasn't found:\n"
                            + "\n".join(l for l in text.splitlines() if "version" in l))
        if 'version of wbt-hang: ""' not in text:
            failures.append("the program that never answers wasn't stopped")
        try:
            pid = int(open(hang_pid).read())
            os.kill(pid, 0)
            failures.append(f"the version command that hung ({pid}) still runs")
            os.kill(pid, signal.SIGKILL)
        except (OSError, ValueError):
            pass
        reached = open(seen).read().strip() if os.path.exists(seen) else ""
        if reached != "WAYLAND= DISPLAY= BUS=disabled:":
            failures.append(f"the version command could reach the session: {reached!r}")
        if os.path.exists(asked_none):
            failures.append("a program without Version= was run")
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
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
    print("integration wbconf versions: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
