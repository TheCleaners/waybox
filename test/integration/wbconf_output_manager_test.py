#!/usr/bin/env python3
"""Integration test: wbconf's notice that kanshi (Manages=outputs) runs.

A dummy daemon named kanshi (it only waits).
1. Started outside Waybox, as an autostart script would: the Monitors
   page's notice offers to stop it for the session; its Stop
   (WBCONF_TEST_SET output-manager-stop=1) ends it, and not a kanshi of
   another session (a decoy, and any of the user's own).
2. In Waybox's startup list: the notice offers to turn it off there
   instead (stopping it alone, Waybox would start it again); saved, the
   program is enabled="no" in rc.xml and Waybox has stopped it.
Skips (exit 77) without wtype or wlr-randr (the monitors page), or a
headless display.

Usage: wbconf_output_manager_test.py <waybox> <wbconf> <schema-dir> <fake_daemon>
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


def kanshis():
    """Every kanshi of ours, in any session."""
    me = os.getuid()
    out = subprocess.run(["ps", "-U", str(me), "-o", "pid=,comm="], capture_output=True,
                         text=True).stdout
    return [int(l.split()[0]) for l in out.splitlines() if l.split()[1:] == ["kanshi"]]


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, daemon = sys.argv[1:]
    for tool in ("wtype", "wlr-randr", "ps"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    # The user's own kanshi (another session) must be left alone: noted
    # before, checked after.
    theirs = set(kanshis())
    workdir = tempfile.mkdtemp(prefix="waybox-wbmgr-", dir="/tmp")
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    fake = os.path.join(bindir, "kanshi")
    shutil.copy(daemon, fake)
    # kanshi's integration file (Manages=outputs), from the source tree.
    share = os.path.join(workdir, "share")
    os.makedirs(os.path.join(share, "waybox", "integrations"))
    shutil.copy(os.path.join(os.path.dirname(os.path.abspath(schema)), "integrations",
                             "kanshi.integration"),
                os.path.join(share, "waybox", "integrations"))
    failures = []

    def run(startup_rc, test_set, then):
        cfg = os.path.join(workdir, "config")
        shutil.rmtree(cfg, ignore_errors=True)
        os.makedirs(os.path.join(cfg, "waybox"))
        rc_path = os.path.join(cfg, "waybox", "rc.xml")
        with open(rc_path, "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{startup_rc}</waybox>
</openbox_config>
""")
        runtime = os.path.join(workdir, "run")
        shutil.rmtree(runtime, ignore_errors=True)
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
                   PATH=bindir + ":" + os.environ.get("PATH", ""), XDG_DATA_DIRS=share,
                   ASAN_OPTIONS="detect_leaks=0")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
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
        if sock is None:
            log.close()
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            proc.kill()
            sys.exit(SKIP)
        cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                    DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                    GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                    WAYBOX_PID=str(proc.pid), WBCONF_TEST_SET=test_set)
        try:
            then(cenv, rc_path, proc)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()

    def wbconf_run(cenv, save):
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        time.sleep(4)
        alive = app.poll() is None
        if alive and save:
            subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                           capture_output=True, timeout=10)
            time.sleep(1.5)
        if app.poll() is None:
            app.kill()
        app.wait()
        if not alive:
            failures.append("wbconf exited")

    # 1. Started outside Waybox: stopped for the session. A decoy kanshi
    # of another session (another runtime directory) is left alone.
    def outside(cenv, rc_path, proc):
        k = subprocess.Popen([fake], env=cenv, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        wbtest.reap_on_exit(k)
        other = os.path.join(workdir, "other-run")
        os.makedirs(other, 0o700, exist_ok=True)
        decoy = subprocess.Popen([fake], env=dict(cenv, XDG_RUNTIME_DIR=other,
                                                  WAYLAND_DISPLAY="wayland-77"),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
        wbtest.reap_on_exit(decoy)
        time.sleep(0.3)
        if k.poll() is not None:
            failures.append("the dummy kanshi didn't start")
            return
        wbconf_run(cenv, save=False)
        time.sleep(0.3)
        if k.poll() is None:
            failures.append("the notice's Stop didn't stop kanshi")
            k.kill()
        k.wait()
        if decoy.poll() is not None:
            failures.append("another session's kanshi was stopped")
        else:
            decoy.kill()
        decoy.wait()
    run("", "output-manager-stop=1", outside)

    # 2. In Waybox's startup list: turned off there, stopped on save.
    def listed(cenv, rc_path, proc):
        # The kanshi Waybox started: from its log (the same on every
        # system, unlike ps's names for what sh -c runs).
        log_path = os.path.join(workdir, "waybox.log")
        pid = None
        for _ in range(50):
            time.sleep(0.1)
            m = re.search(r"wb-startup start kanshi (\d+)", open(log_path).read())
            if m:
                pid = int(m.group(1))
                break
        if pid is None:
            failures.append("Waybox didn't start the listed kanshi")
            return
        wbconf_run(cenv, save=True)
        time.sleep(1.5)
        rc = open(rc_path).read()
        if 'enabled="no"' not in rc:
            failures.append(f"kanshi wasn't turned off in rc.xml:\n{rc}")
        if "wb-startup stop kanshi" not in open(log_path).read():
            failures.append("Waybox didn't stop kanshi after the save")
        try:
            os.kill(pid, 0)
            failures.append(f"kanshi ({pid}) still runs after the save")
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    run('<startup><program name="kanshi">kanshi</program></startup>',
        "output-manager-stop=1", listed)

    gone = theirs - set(kanshis())
    if gone:
        failures.append(f"a kanshi of another session was stopped: {sorted(gone)}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf output manager: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
