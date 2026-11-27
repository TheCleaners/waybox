#!/usr/bin/env python3
"""Headless integration test: ShowMenu's <position> (as Openbox).

Two monitors side by side. F5: x and y "center" on monitor 2: the menu
centered on the right one. F6: x 10, y -10, no monitor: 10 from the left
and the bottom of the primary (the leftmost). F7: only x: at the pointer
(Openbox needs both). F8: monitor "all": centered on the two together.

Skips (exit 77) when wtype, wlrctl, wlr-randr or a headless display are
unavailable.

Usage: menu_position_test.py <waybox-binary>
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
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wtype", "wlrctl", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-mp-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)

    def show(key, position):
        return (f'    <keybind key="{key}"><action name="ShowMenu"><menu>root-menu</menu>'
                f'{position}</action></keybind>\n')
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc">\n  <keyboard>\n'
                + show("F5", "<position><x>center</x><y>center</y><monitor>2</monitor></position>")
                + show("F6", "<position><x>10</x><y>-10</y></position>")
                + show("F7", "<position><x>center</x></position>")
                + show("F8", "<position><x>center</x><y>center</y><monitor>all</monitor></position>")
                + '  </keyboard>\n</openbox_config>\n')
    with open(os.path.join(cfg, "menu.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_menu xmlns="http://openbox.org/3.4/menu">\n'
                '  <menu id="root-menu" label="Root">\n'
                '    <item label="One"><action name="Execute"><command>true</command></action></item>\n'
                '    <item label="Two"><action name="Execute"><command>true</command></action></item>\n'
                '  </menu>\n</openbox_menu>\n')

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
    rc = 0
    placed = {}
    try:
        time.sleep(0.5)
        outs = subprocess.run(["wlr-randr"], env=cenv, capture_output=True, text=True).stdout
        positions = sorted(tuple(map(int, p)) for p in
                           re.findall(r"Position: (-?\d+),(-?\d+)", outs))
        sizes = re.findall(r"(\d+)x(\d+) px[^\n]*current", outs)
        if len(positions) != 2 or not sizes:
            raise RuntimeError(f"expected two outputs:\n{outs}")
        W, H = map(int, sizes[0])
        if positions != [(0, 0), (W, 0)]:
            raise RuntimeError(f"expected them side by side: {positions}")
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", "300", "200"], env=cenv,
                       capture_output=True)
        for key in ("F5", "F6", "F7", "F8"):
            subprocess.run(["wtype", "-k", key], env=cenv, capture_output=True)
            time.sleep(0.6)
            text = open(log_path).read()
            at = re.findall(r"wb-menu at (-?\d+),(-?\d+) (\d+)x(\d+)", text)
            placed[key] = tuple(map(int, at[-1])) if len(at) == len(placed) + 1 else None
            subprocess.run(["wtype", "-k", "Escape"], env=cenv, capture_output=True)
            time.sleep(0.4)

        def check(key, want_x, want_y):
            got = placed.get(key)
            if got is None or (got[0], got[1]) != (want_x, want_y):
                failures.append(f"{key}: the menu at {got}, not at {want_x},{want_y}")
        if placed.get("F5"):
            mw, mh = placed["F5"][2], placed["F5"][3]
            check("F5", W + (W - mw) // 2, (H - mh) // 2)
            check("F6", 10, H - mh - 10)
            check("F7", 300, 200)
            check("F8", (2 * W - mw) // 2, (H - mh) // 2)
        else:
            failures.append(f"no menu opened: {placed}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
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
    print("integration menu_position: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
