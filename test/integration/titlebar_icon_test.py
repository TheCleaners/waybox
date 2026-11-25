#!/usr/bin/env python3
"""Integration test for the titlebar icon (<titleLayout> N).

Icons, in a private data directory:
- a red 32 px "wbtheme" icon in the hicolor theme;
- a desktop file for "wbdesk" whose Icon= is a green PNG's path.

foot sets its app ID as its icon name (xdg-toplevel-icon), so:

1. foot -a wbtheme: the titlebar's left end shows the red themed icon.
2. foot -a wbdesk: there's no "wbdesk" icon, so the desktop file's green
   icon shows.
3. Clicking the icon opens the window menu.
4. With <titleLayout>LIMC</titleLayout> there is no icon.

Skips (exit 77) without foot, wlrctl, grim or magick, or a headless display.

Usage: titlebar_icon_test.py <waybox-binary>
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
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "wlrctl", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-ticon-", dir="/tmp")
    data = os.path.join(workdir, "data")
    themed = os.path.join(data, "icons", "hicolor", "32x32", "apps")
    os.makedirs(themed)
    os.makedirs(os.path.join(data, "applications"))
    subprocess.run(["magick", "-size", "32x32", "xc:#ff0000",
                    os.path.join(themed, "wbtheme.png")], check=True)
    # A hicolor index of its own: the test mustn't depend on the host's
    # (CI containers may not have the hicolor theme installed).
    with open(os.path.join(data, "icons", "hicolor", "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=Hicolor\nDirectories=32x32/apps\n\n"
                "[32x32/apps]\nSize=32\nContext=Applications\nType=Threshold\n")
    green = os.path.join(workdir, "green.png")
    subprocess.run(["magick", "-size", "32x32", "xc:#00ff00", green], check=True)
    with open(os.path.join(data, "applications", "wbdesk.desktop"), "w") as f:
        f.write(f"[Desktop Entry]\nType=Application\nName=Desk\nExec=true\nIcon={green}\n")
    foot_ini = os.path.join(workdir, "foot.ini")
    with open(foot_ini, "w") as f:
        f.write("[colors-dark]\nbackground=0000ff\nforeground=0000ff\n")

    failures = []
    logs = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def session(layout, apps, check):
        cfg = os.path.join(workdir, f"cfg-{layout}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <theme><titleLayout>{layout}</titleLayout></theme>
</openbox_config>
""")
        runtime = os.path.join(workdir, f"run-{layout}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, XDG_DATA_HOME=data,
                   XDG_DATA_DIRS="/usr/share", WB_ICON_THEME="hicolor",
                   WB_RC_XML="", WB_MENU_XML="",
                   ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, f"waybox-{layout}.log")
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
        try:
            for app in apps:
                foot = subprocess.Popen(["foot", "-c", foot_ini, "-a", app], env=cenv,
                                        stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
                time.sleep(1.5)
                log.flush()
                geo = re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)",
                                 open(log_path).read())[-1]
                w, h, x, y = map(int, geo)
                check(app, cenv, (x, y, w, h), log_path)
                foot.terminate()
                foot.wait()
                time.sleep(0.4)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"{layout}: waybox exit status {proc.returncode}")

    def count(cenv, box, colour):
        """Pixels of `colour` in the titlebar strip's left 60 px."""
        x, y, w, h = box
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", "-g", f"{x},{max(y - 40, 0)} 60x40", shot], env=cenv,
                       capture_output=True)
        out = subprocess.run(["magick", shot, "-fill", "black", "+opaque", colour,
                              "-fill", "white", "-opaque", colour, "-format",
                              "%[fx:int(mean*w*h+0.5)]", "info:"],
                             capture_output=True, text=True).stdout.strip()
        return int(out or 0)

    def with_icon(app, cenv, box, log_path):
        if app == "wbtheme":
            expect(count(cenv, box, "#ff0000") > 100,
                   "the themed icon isn't in the titlebar")
        else:
            expect(count(cenv, box, "#00ff00") > 100,
                   "the desktop file's icon isn't in the titlebar")
        # 3. Click the icon: the window menu.
        x, y, w, h = box
        icon_y = y - 12
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x + 12), str(icon_y)], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        time.sleep(0.5)
        expect(f"wb-menu client-menu {app}" in open(log_path).read(),
               f"clicking {app}'s icon didn't open the window menu")
        subprocess.run(["wlrctl", "keyboard", "type", "\x1b"], env=cenv,
                       capture_output=True)

    def without_icon(app, cenv, box, log_path):
        expect(count(cenv, box, "#ff0000") == 0,
               "with LIMC the titlebar still shows an icon")

    session("NLIMC", ["wbtheme", "wbdesk"], with_icon)
    session("LIMC", ["wbtheme"], without_icon)

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
    print("integration titlebar icon: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
