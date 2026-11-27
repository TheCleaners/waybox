#!/usr/bin/env python3
"""Integration test: identify (each monitor's name, large, on it).

A headless Waybox with two outputs and the scripting socket:
- waybox-msg identify: a popup with its name on each monitor (2 shown),
  which goes after the time asked;
- identify HEADLESS-2: on that one only; identify NOPE: an error;
- wbconf's Identify button (WBCONF_TEST_SET identify=1) asks Waybox;
- without the socket, wbconf starts all the same (its button greyed).
The popup is checked in the picture too: the middle of each output isn't
the background any more.
Skips (exit 77) without grim, or a headless display.

Usage: identify_test.py <waybox> <waybox-msg> <wbconf> <schema-dir>
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><ipc enabled="{ipc}"/></waybox>
</openbox_config>
"""


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, msg, wbconf, schema = sys.argv[1:]
    if shutil.which("grim") is None:
        print("SKIP: grim not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-identify-", dir="/tmp")
    failures = []

    def start(ipc, tag):
        cfg = os.path.join(workdir, f"config-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(RC.format(ipc=ipc))
        runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   WLR_HEADLESS_OUTPUTS="2", HOME=workdir, XDG_CONFIG_HOME=cfg,
                   XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
                   ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
                   + "exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, f"waybox-{tag}.log")
        log = open(log_path, "w")
        proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
        wbtest.reap_on_exit(proc)
        for _ in range(50):
            time.sleep(0.1)
            if proc.poll() is not None:
                break
            socks = [n for n in os.listdir(runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                return proc, log, log_path, dict(env, WAYLAND_DISPLAY=socks[0])
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    def stop(proc, log):
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")

    def ask(cenv, *args):
        out = subprocess.run([msg, *args], env=cenv, capture_output=True, text=True, timeout=10)
        try:
            return json.loads(out.stdout)
        except ValueError:
            return {"raw": out.stdout + out.stderr}

    def centre_pixels(cenv):
        """A 300x100 area in the middle of each output (as PPM: no image
        library), to compare."""
        pix = {}
        outs = {o["name"]: o for o in ask(cenv, "get_outputs").get("outputs", [])}
        for name, o in outs.items():
            cx, cy = o["x"] + o["width"] // 2 - 150, o["y"] + o["height"] // 2 - 50
            r = subprocess.run(["grim", "-t", "ppm", "-g", f"{cx},{cy} 300x100", "-"],
                               env=cenv, capture_output=True, timeout=10)
            pix[name] = r.stdout[-300 * 100 * 3:] if r.returncode == 0 else None
        return pix

    proc, log, log_path, cenv = start("yes", "ipc")
    try:
        before = centre_pixels(cenv)
        r = ask(cenv, "identify")
        if r.get("shown") != 2:
            failures.append(f"identify: {r}")
        time.sleep(0.3)
        during = centre_pixels(cenv)
        for name in ("HEADLESS-1", "HEADLESS-2"):
            if before.get(name) is None or before.get(name) == during.get(name):
                failures.append(f"nothing shown in the middle of {name}")
        log.flush()
        text = open(log_path).read()
        for name in ("HEADLESS-1", "HEADLESS-2"):
            if f"wb-desktop-popup {name}" not in text:
                failures.append(f"no popup for {name} in the log")
        time.sleep(3.2)  # the default 3 s
        after = centre_pixels(cenv)
        if after != before:
            failures.append("the names didn't go")
        r = ask(cenv, "identify", "HEADLESS-2")
        if r.get("shown") != 1:
            failures.append(f"identify HEADLESS-2: {r}")
        r = ask(cenv, "identify", "NOPE")
        if "no such monitor" not in r.get("raw", ""):
            failures.append(f"identify NOPE: {r}")
        r = ask(cenv, "raw", '{"request": "identify", "ms": 200}')
        if r.get("shown") != 2:
            failures.append(f"identify with ms: {r}")

        # wbconf's button.
        wenv = dict(cenv, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                    DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                    GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                    WAYBOX_PID=str(proc.pid), WBCONF_TEST_SET="identify=1")
        log.flush()
        count = open(log_path).read().count("wb-identify ")
        app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        time.sleep(4)
        alive = app.poll() is None
        if alive:
            app.kill()
        app.wait()
        if not alive:
            failures.append("wbconf exited")
        log.flush()
        if open(log_path).read().count("wb-identify ") != count + 1:
            failures.append("wbconf's Identify didn't ask Waybox")
    finally:
        stop(proc, log)

    # Without the socket: wbconf still starts (Identify greyed).
    proc, log, log_path, cenv = start("no", "noipc")
    try:
        wenv = dict(cenv, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                    DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                    GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                    WAYBOX_PID=str(proc.pid), WBCONF_TEST_SET="identify=1")
        app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        time.sleep(3)
        if app.poll() is not None:
            failures.append(f"wbconf without the socket exited ({app.returncode})")
        else:
            app.kill()
        app.wait()
    finally:
        stop(proc, log)

    for lp in (os.path.join(workdir, "waybox-ipc.log"), os.path.join(workdir, "waybox-noipc.log")):
        if any(s in open(lp).read() for s in ("Sanitizer", "Assertion", "runtime error")):
            failures.append(f"sanitizer/assert output in {os.path.basename(lp)}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration identify: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
