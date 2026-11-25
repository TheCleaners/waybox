#!/usr/bin/env python3
"""Integration test: window icons in the Alt+Tab switcher.

Two foot windows whose app IDs have red icons (in a private icon theme;
titlebar icons off, so red can only come from the switcher). While Alt is
held after a Tab:

1. The switcher, in the middle of the screen, shows the icons (red).
2. With <switcher icons="no"> it shows none.

Skips (exit 77) without foot, wtype, grim or magick, or a headless display.

Usage: switcher_icons_test.py <waybox-binary>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wtype", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-swicon-", dir="/tmp")
    data = os.path.join(workdir, "data")
    apps = os.path.join(data, "icons", "hicolor", "32x32", "apps")
    os.makedirs(apps)
    with open(os.path.join(data, "icons", "hicolor", "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=Hicolor\nDirectories=32x32/apps\n\n"
                "[32x32/apps]\nSize=32\nContext=Applications\nType=Threshold\n")
    for name in ("wbone", "wbtwo"):
        subprocess.run(["magick", "-size", "32x32", "xc:#ff0000",
                        os.path.join(apps, name + ".png")], check=True)
    foot_ini = os.path.join(workdir, "foot.ini")
    with open(foot_ini, "w") as f:
        f.write("[colors-dark]\nbackground=0000ff\nforeground=0000ff\n")
    failures = []
    logs = []

    def red_in_switcher(icons):
        cfg = os.path.join(workdir, f"cfg-{icons}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><titleLayout>LIMC</titleLayout></theme>
  <keyboard>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
  </keyboard>
  <waybox><switcher icons="{icons}"/></waybox>
</openbox_config>
""")
        runtime = os.path.join(workdir, f"run-{icons}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, XDG_DATA_HOME=data,
                   XDG_DATA_DIRS="/nonexistent", WB_ICON_THEME="hicolor",
                   WB_RC_XML="", WB_MENU_XML="",
                   ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, f"waybox-{icons}.log")
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
        feet = []
        red = -1
        try:
            for app in ("wbone", "wbtwo"):
                feet.append(subprocess.Popen(["foot", "-c", foot_ini, "-a", app], env=cenv,
                                             stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL))
                time.sleep(1.2)
            # Hold Alt after a Tab, so the switcher stays up for the shot.
            hold = subprocess.Popen(["wtype", "-M", "alt", "-k", "Tab", "-s", "1500",
                                     "-m", "alt"], env=cenv, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            time.sleep(0.7)
            shot = os.path.join(workdir, f"shot-{icons}.png")
            subprocess.run(["grim", "-g", "440,260 400x200", shot], env=cenv,
                           capture_output=True)
            hold.wait(timeout=5)
            out = subprocess.run(["magick", shot, "-fill", "black", "+opaque", "#ff0000",
                                  "-fill", "white", "-opaque", "#ff0000", "-format",
                                  "%[fx:int(mean*w*h+0.5)]", "info:"],
                                 capture_output=True, text=True).stdout.strip()
            red = int(out or 0)
        finally:
            for f in feet:
                f.kill()
            time.sleep(0.3)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"icons={icons}: waybox exit status {proc.returncode}")
        return red

    with_icons = red_in_switcher("yes")
    if with_icons < 100:
        failures.append(f"the switcher shows no icons ({with_icons} red pixels)")
    without = red_in_switcher("no")
    if without != 0:
        failures.append(f'with icons="no" the switcher still shows icons ({without})')

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
    print("integration switcher icons: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
