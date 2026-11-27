#!/usr/bin/env python3
"""Headless integration test: desktops per monitor (<perMonitor>yes, #367).

Two monitors side by side; a window on each ("left", "right"), both on
desktop 1. With "right" focused, GoToDesktop 2 switches only the right
monitor: "right" hidden, "left" still shown. "left" focused, GoToDesktop
3: the left monitor alone, "left" hidden. Nothing focused there now, the
pointer's monitor (the left) goes back to 1: "left" shown. A window sent
to monitor 2 (MoveResizeTo) takes the desktop that one shows (2), and is
shown there; the right monitor's own window stays hidden. A new window
opened on the right monitor opens on its desktop (2).

Skips (exit 77) without foot, wtype, wlrctl, wlr-randr or a headless
display.

Usage: desktops_per_monitor_test.py <waybox> <waybox-msg>
"""
import json
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
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, msg = sys.argv[1:]
    for tool in ("foot", "wtype", "wlrctl", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-dpm-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)

    def key(k, action):
        return f'    <keybind key="{k}">{action}</keybind>\n'
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc">\n'
                '  <desktops><number>3</number><perMonitor>yes</perMonitor></desktops>\n'
                '  <keyboard>\n'
                + key("F1", '<action name="GoToDesktop"><to>1</to></action>')
                + key("F2", '<action name="GoToDesktop"><to>2</to></action>')
                + key("F3", '<action name="GoToDesktop"><to>3</to></action>')
                + key("F6", '<action name="MoveResizeTo"><monitor>2</monitor>'
                            '<x>center</x><y>center</y></action>')
                + '  </keyboard>\n</openbox_config>\n')

    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        WLR_HEADLESS_OUTPUTS="2",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        WB_RC_XML="",
        WB_MENU_XML="",
        ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99",
    )
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)

    display = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        for name in os.listdir(runtime):
            if name.startswith("wayland-") and not name.endswith(".lock"):
                display = name
                break
        if display:
            break
    if display is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    failures = []
    clients = []
    rc = 0

    def windows():
        out = subprocess.run([msg, "get_windows"], env=cenv, capture_output=True, text=True)
        try:
            return {w["app_id"]: w for w in json.loads(out.stdout)["windows"]}
        except (ValueError, KeyError):
            return {}

    def pointer(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=cenv,
                       capture_output=True)
        time.sleep(0.2)

    def press(k):
        subprocess.run(["wtype", "-k", k], env=cenv, capture_output=True)
        time.sleep(0.5)

    def open_foot(app_id):
        p = subprocess.Popen(["foot", "--app-id", app_id], env=cenv,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(p)
        clients.append(p)
        for _ in range(50):
            time.sleep(0.1)
            if app_id in windows():
                break
        time.sleep(0.3)

    def expect(step, app_id, desktop, shown):
        w = windows().get(app_id)
        if w is None or w.get("desktop") != desktop or w.get("shown") != shown:
            failures.append(f"{step}: {app_id} is "
                            f"{None if w is None else (w.get('desktop'), w.get('shown'), w.get('output'))}"
                            f", not desktop {desktop} {'shown' if shown else 'hidden'}")

    try:
        time.sleep(0.5)
        outs = subprocess.run(["wlr-randr"], env=cenv, capture_output=True, text=True).stdout
        positions = sorted(tuple(map(int, p)) for p in
                           re.findall(r"Position: (-?\d+),(-?\d+)", outs))
        sizes = re.findall(r"(\d+)x(\d+) px[^\n]*current", outs)
        if len(positions) != 2 or not sizes:
            raise RuntimeError(f"expected two outputs:\n{outs}")
        W = int(sizes[0][0])

        pointer(300, 300)
        open_foot("left")
        pointer(W + 300, 300)
        open_foot("right")
        expect("opened", "left", 1, True)
        expect("opened", "right", 1, True)

        # "right" focused: only the right monitor goes to 2.
        press("F2")
        expect("right monitor to 2", "right", 1, False)
        expect("right monitor to 2", "left", 1, True)

        # "left" focused: only the left monitor goes to 3.
        pointer(300, 300)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        time.sleep(0.3)
        press("F3")
        expect("left monitor to 3", "left", 1, False)
        expect("left monitor to 3", "right", 1, False)

        # Nothing focused on it: the pointer's monitor (the left) back to 1.
        press("F1")
        expect("left monitor back to 1", "left", 1, True)
        expect("left monitor back to 1", "right", 1, False)

        # "left" sent to monitor 2, which shows desktop 2: it goes along.
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        time.sleep(0.3)
        press("F6")
        expect("moved to the right monitor", "left", 2, True)
        expect("moved to the right monitor", "right", 1, False)

        # A new window on the right monitor: on the desktop it shows (2).
        pointer(W + 300, 300)
        open_foot("new")
        expect("opened on the right monitor", "new", 2, True)
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for p in clients:
            p.terminate()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
        sys.stderr.write(contents)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration desktops_per_monitor: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
