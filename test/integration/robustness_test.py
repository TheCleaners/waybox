#!/usr/bin/env python3
"""Integration test: things that used to hang or crash Waybox.

1. Two maximized windows, then a panel appears (re-fitting windows):
   Waybox stays responsive (it used to loop forever, re-raising them).
2. A mouse binding whose action is Reconfigure, with more bindings after
   it: no use of the freed old bindings (checked under ASan).
3. With no settings loaded (an empty rc.xml), clicking a titlebar icon
   (the window menu) doesn't crash.
4. A failed start (no usable runtime directory) exits cleanly instead of
   freeing the backend and display twice.

Skips (exit 77) without foot or wlrctl, or a headless display.

Usage: robustness_test.py <waybox-binary> <layer_client>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, layer_client = sys.argv[1:]
    for tool in ("foot", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-robust-", dir="/tmp")
    failures = []
    logs = []

    def session(tag, rc, steps):
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(rc)
        runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
                   WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, f"waybox-{tag}.log")
        logs.append(log_path)
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
        cenv = dict(env, WAYLAND_DISPLAY=sock)
        children = []

        def spawn(*argv):
            p = subprocess.Popen(list(argv), env=cenv, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            children.append(p)
            return p

        def wlrctl(*args, timeout=5):
            try:
                return subprocess.run(["wlrctl", *args], env=cenv, capture_output=True,
                                      timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                return None

        def logtext():
            log.flush()
            return open(log_path).read()

        try:
            steps(spawn, wlrctl, logtext)
            if proc.poll() is not None:
                failures.append(f"{tag}: waybox exited (status {proc.returncode})")
        finally:
            for c in children:
                if c.poll() is None:
                    c.kill()
            time.sleep(0.3)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    failures.append(f"{tag}: waybox did not exit on SIGTERM (hung?)")
            log.close()
            if proc.returncode not in (0, None, -9):
                failures.append(f"{tag}: waybox exit status {proc.returncode}")

    empty_rc = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc"/>
"""

    # 1. Two maximized windows, then a panel.
    def maximized_then_panel(spawn, wlrctl, logtext):
        for app in ("one", "two"):
            spawn("foot", "-a", app)
            time.sleep(1.0)
            wlrctl("toplevel", "maximize", f"app_id:{app}")
            time.sleep(0.3)
        spawn(layer_client)
        time.sleep(1.5)
        if wlrctl("toplevel", "find", "app_id:one", timeout=3) != 0:
            failures.append("maximized windows + a panel: Waybox stopped responding")
        n = logtext().count("maximize-on")
        if n > 20:
            failures.append(f"maximized windows re-fitted {n} times (a loop)")
    session("maximized", empty_rc, maximized_then_panel)

    # 2. Reconfigure from a mouse binding, with more bindings after it.
    reconf_rc = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse><context name="Root">
    <mousebind button="Middle" action="Press"><action name="Reconfigure"/></mousebind>
    <mousebind button="Middle" action="Press"><action name="Execute"><command>true</command></action></mousebind>
    <mousebind button="Middle" action="Press"><action name="Execute"><command>true</command></action></mousebind>
  </context></mouse>
</openbox_config>
"""
    def reconfigure_binding(spawn, wlrctl, logtext):
        wlrctl("pointer", "move", "600", "400")
        for _ in range(3):
            wlrctl("pointer", "click", "middle")
            time.sleep(0.4)
        if logtext().count("Reconfigured") < 1:
            failures.append("the Reconfigure binding didn't run")
    session("reconfigure", reconf_rc, reconfigure_binding)

    # 3. No settings: click a titlebar icon (the window menu).
    def icon_without_settings(spawn, wlrctl, logtext):
        spawn("foot", "-a", "solo")
        time.sleep(1.2)
        geo = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())
        if not geo:
            failures.append("no settings: the window never mapped")
            return
        _, _, x, y = map(int, geo[-1])
        wlrctl("pointer", "move", "-5000", "-5000")
        wlrctl("pointer", "move", str(x + 10), str(y - 10))
        time.sleep(0.2)
        wlrctl("pointer", "click", "left")
        time.sleep(0.5)
    os.makedirs(os.path.join(workdir, "empty"), exist_ok=True)
    session("nosettings", "", icon_without_settings)

    # 4. Startup failure (no usable runtime directory, so no socket): a
    # clean exit, not a double free of the backend and display.
    bad_runtime = os.path.join(workdir, "not-a-dir")
    open(bad_runtime, "w").close()
    start_log = os.path.join(workdir, "waybox-start.log")
    logs.append(start_log)
    with open(start_log, "w") as f:
        r = subprocess.run([waybox, "--debug"], stdout=f, stderr=subprocess.STDOUT,
                           timeout=30, env=dict(os.environ, WLR_BACKENDS="headless",
                                                WLR_RENDERER="pixman",
                                                XDG_RUNTIME_DIR=bad_runtime,
                                                WB_RC_XML="/dev/null", WB_MENU_XML="",
                                                ASAN_OPTIONS="detect_leaks=1:exitcode=99"))
    if r.returncode < 0 or r.returncode == 99:
        failures.append(f"a failed start crashed (status {r.returncode})")

    contents = "".join(open(p).read() for p in logs)
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration robustness: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
