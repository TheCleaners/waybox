#!/usr/bin/env python3
"""Integration test: the waybox Python module against a headless Waybox,
and the Focus action through it.

1. version(), windows(), desktops(), outputs() match Waybox's state.
2. Focus: the older of two windows gets focus; one on another desktop
   gets focus and its desktop is shown.
3. subscribe(): a new window's event arrives.
4. Errors: a window that doesn't exist raises RequestFailed.
Skips (77) without a headless display.

Usage: python_ipc_test.py <waybox> <touch_client> <python-source-dir>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

import wbtest

SKIP = 77


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox_bin, client_bin, pydir = sys.argv[1:]
    sys.path.insert(0, pydir)
    import waybox

    workdir = tempfile.mkdtemp(prefix="waybox-pyipc-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?><openbox_config xmlns="http://openbox.org/3.4/rc">'
                '<desktops><number>2</number></desktops>'
                '<waybox><ipc enabled="yes"/></waybox></openbox_config>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    env.pop("WAYBOX_SOCK", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox_bin, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
    # The module finds the socket as a program in the session would.
    os.environ.pop("WAYBOX_SOCK", None)
    os.environ["XDG_RUNTIME_DIR"] = runtime
    os.environ["WAYLAND_DISPLAY"] = sock
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    clients = []

    def window(app_id, wb):
        return next((w for w in wb.windows() if w["app_id"] == app_id), None)

    try:
        with waybox.connect() as wb:
            if not wb.version():
                failures.append("no version")
            # Two windows: touch-yes first, then touch-no (focused).
            for kind in ("yes", "no"):
                c = subprocess.Popen([client_bin, kind], env=cenv, stdout=subprocess.DEVNULL)
                wbtest.reap_on_exit(c)
                clients.append(c)
                time.sleep(0.6)
            ws = wb.windows()
            if sorted(w["app_id"] for w in ws) != ["touch-no", "touch-yes"]:
                failures.append(f"windows: {ws}")
            if not window("touch-no", wb)["focused"]:
                failures.append("the newer window isn't focused")
            if [d["current"] for d in wb.desktops()] != [True, False]:
                failures.append(f"desktops: {wb.desktops()}")
            if not wb.outputs():
                failures.append("no outputs")

            # 2. Focus.
            wb.focus(window("touch-yes", wb)["id"])
            time.sleep(0.3)
            if not window("touch-yes", wb)["focused"]:
                failures.append("Focus didn't focus the window")
            wb.send_to_desktop(2, window="app_id:touch-no", follow=False)
            time.sleep(0.3)
            if window("touch-no", wb)["desktop"] != 2:
                failures.append("SendToDesktop didn't move it")
            wb.focus("app_id:touch-no")
            time.sleep(0.3)
            if not window("touch-no", wb)["focused"]:
                failures.append("Focus didn't focus a window on another desktop")
            if [d["current"] for d in wb.desktops()] != [False, True]:
                failures.append(f"Focus didn't show its desktop: {wb.desktops()}")

            # 4. Errors.
            try:
                wb.close_window("no-such-id")
                failures.append("a missing window didn't raise")
            except waybox.RequestFailed as e:
                if "no such window" not in str(e):
                    failures.append(f"error message: {e}")

        # 3. Events, from a connection of their own.
        got = []

        def listen():
            for e in waybox.subscribe(["window"]):
                if e.get("change") == "new":
                    got.append(e)
                    return
        t = threading.Thread(target=listen, daemon=True)
        t.start()
        time.sleep(0.3)
        c = subprocess.Popen([client_bin, "yes"], env=cenv, stdout=subprocess.DEVNULL)
        wbtest.reap_on_exit(c)
        clients.append(c)
        t.join(5)
        if not got or got[0]["window"].get("app_id") != "touch-yes":
            failures.append(f"no event for the new window: {got}")
    except (waybox.WayboxError, OSError, KeyError, TypeError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        for c in clients:
            c.kill()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    text = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in text for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration python ipc: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
