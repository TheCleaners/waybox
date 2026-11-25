#!/usr/bin/env python3
"""Integration test: wbconf moves a program from the autostart script to
Waybox's <startup> when it's turned on there.

The autostart script (run as the session would) starts "wbt-notify" on a line of
its own, and "wbt-wall" from a function's loop that keeps running, as
wallpaper changers do. Both are turned on in wbconf (WBCONF_TEST_SET) and
the question answered "let Waybox start it, and stop the running copy"
(WBCONF_TEST_CHOICE); saved with Ctrl+S:
- both script lines are commented out, nothing else changes;
- rc.xml's <startup> has both, the line's own with its arguments;
- the copies the script started are stopped, and the function's loop with
  them (or it would start another), but not other processes;
- Waybox, reloaded, starts its own.
Then a program turned on and left to the script: switched back off,
nothing written, nothing stopped.
Skips (77) without wtype or a headless display.

Usage: wbconf_move_test.py <waybox> <wbconf> <schema-dir> <fake_daemon>
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

SCRIPT = """#!/bin/sh
command -v wbt-notify >/dev/null 2>&1 && wbt-notify --anchor top-left &
wallpaper_loop() {
    while :; do
        wbt-wall -m fill &
        sleep 300
    done
}
wallpaper_loop &
wbt-sun &
"""
EXPECTED = """#!/bin/sh
#command -v wbt-notify >/dev/null 2>&1 && wbt-notify --anchor top-left &
wallpaper_loop() {
    while :; do
        wbt-wall -m fill &
        sleep 300
    done
}
#wallpaper_loop &
wbt-sun &
"""

INTEGRATION = "[Integration]\nName={name}\nCategory=Other\nComment=test\nExec={name}\n"


def pids_named(name):
    out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "comm="], capture_output=True,
                         text=True).stdout
    return {int(p) for p, c in (l.split(None, 1) for l in out.splitlines() if l.strip())
            if c.strip() == name}


def pids_running(path):
    """Processes whose arguments include `path` (a script's shells: named
    after it on Linux, after the shell on the BSDs)."""
    out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "args="], capture_output=True,
                         text=True).stdout
    return {int(l.split(None, 1)[0]) for l in out.splitlines()
            if len(l.split(None, 1)) == 2 and path in l.split(None, 1)[1].split()}


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    r = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True)
    return bool(r.stdout.strip()) and not r.stdout.strip().startswith("Z")


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, fake = sys.argv[1:]
    if shutil.which("wtype") is None or shutil.which("ps") is None:
        print("SKIP: wtype or ps not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbmove-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    bindir = os.path.join(workdir, "bin")
    for d in (os.path.join(cfg, "waybox", "integrations"), bindir):
        os.makedirs(d)
    for name in ("wbt-notify", "wbt-wall", "wbt-sun"):
        shutil.copy(fake, os.path.join(bindir, name))  # runs as `name`
        with open(os.path.join(cfg, "waybox", "integrations", f"{name}.integration"), "w") as f:
            f.write(INTEGRATION.format(name=name))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    script_path = os.path.join(cfg, "waybox", "autostart")
    with open(script_path, "w") as f:
        f.write(SCRIPT)
    os.chmod(script_path, 0o755)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    path = bindir + ":" + os.environ.get("PATH", "/usr/bin:/bin")
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", PATH=path, ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)

    # Made-up names, so nothing of a real session can match; still, any
    # process that has one before the test isn't ours, and must survive.
    theirs = {n: pids_named(n) for n in ("wbt-notify", "wbt-wall", "wbt-sun")}

    # Its own process group, and none of our output: what it leaves
    # running (the loop's sleep) mustn't hold the test's output open, and
    # goes with the group at the end.
    script = subprocess.Popen([script_path], env=env, start_new_session=True,
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    script.wait(timeout=10)  # it starts them in the background and ends
    time.sleep(1.0)
    started = {n: pids_named(n) - theirs[n] for n in ("wbt-notify", "wbt-wall", "wbt-sun")}
    loops = pids_running(script_path)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
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
    failures = []
    leftovers = set()
    try:
        if sock is None:
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            sys.exit(SKIP)
        if not all(len(started[n]) == 1 for n in started) or len(loops) != 1:
            raise RuntimeError(f"the script didn't start its programs: {started} {loops}")
        leftovers |= set().union(*started.values()) | loops
        cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                    DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                    WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid))

        def run_wbconf(test_set, choice):
            app = subprocess.Popen([wbconf], env=dict(cenv, WBCONF_TEST_SET=test_set,
                                                      WBCONF_TEST_CHOICE=choice),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wbtest.reap_on_exit(app)
            time.sleep(4)
            if app.poll() is not None:
                raise RuntimeError(f"wbconf exited ({app.returncode})")
            subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                           capture_output=True, timeout=10)
            time.sleep(2.0)
            app.terminate()
            app.wait(timeout=5)

        run_wbconf("program.wbt-notify=on;program.wbt-wall=on", "waybox+stop")
        out = open(script_path).read()
        if out != EXPECTED:
            failures.append(f"the script isn't as expected:\n{out}")
        rc = open(rc_path).read()
        for name in ("wbt-notify", "wbt-wall"):
            if not re.search(rf"<program[^>]*>\s*(<command>)?{name}", rc):
                failures.append(f"{name} isn't in <startup>:\n{rc}")
        # The script's own command, with its arguments (not the guard).
        if "wbt-notify --anchor top-left" not in rc or "command -v" in rc:
            failures.append(f"<startup> doesn't run the script's command:\n{rc}")
        for name in ("wbt-notify", "wbt-wall"):
            for pid in started[name]:
                if alive(pid):
                    failures.append(f"the script's {name} ({pid}) is still running")
        for pid in loops:
            if alive(pid):
                failures.append(f"the function's loop ({pid}) is still running")
        for pid in started["wbt-sun"]:
            if not alive(pid):
                failures.append("wbt-sun, not moved, was stopped too")
        for name, pids in theirs.items():
            for pid in pids:
                if not alive(pid):
                    failures.append(f"another {name} ({pid}) was stopped")
        time.sleep(1.0)
        for name in ("wbt-notify", "wbt-wall"):
            mine = pids_named(name) - theirs[name] - started[name]
            leftovers |= mine
            if not mine:
                failures.append(f"Waybox didn't start its own {name}")

        # Left to the script: back off, nothing written, nothing stopped.
        rc_before = open(rc_path).read()
        run_wbconf("program.wbt-sun=on", "script")
        if open(script_path).read() != EXPECTED:
            failures.append("the script changed when left to it")
        if "wbt-sun" in open(rc_path).read() and "wbt-sun" not in rc_before:
            failures.append("wbt-sun was added to <startup> though left to the script")
        for pid in started["wbt-sun"]:
            if not alive(pid):
                failures.append("wbt-sun was stopped though left to the script")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        for pid in leftovers:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            os.killpg(script.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf move: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
