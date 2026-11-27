#!/usr/bin/env python3
"""Integration test: Openbox's window layers (always on top, normal, always
on bottom).

Two foot windows in the same place, red and blue; the screen's colour where
they overlap says which is on top.
1. <applications> puts red always on top: blue, opened after it (focused,
   raised), stays under it.
2. SendToNormalLayer on red, then blue raised: blue on top.
3. ToggleAlwaysOnBottom on blue, then blue raised: red still on top (blue
   raises only within its layer). get_windows says each one's layer.
Skips (exit 77) without foot, grim or magick, or a headless display.

Usage: window_layers_test.py <waybox>
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77
RED = "srgb(255,0,0)"
BLUE = "srgb(0,0,255)"

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <application class="red"><position force="yes"><x>100</x><y>100</y></position>
      <size><width>500</width><height>400</height></size><layer>above</layer></application>
    <application class="blue"><position force="yes"><x>100</x><y>100</y></position>
      <size><width>500</width><height>400</height></size></application>
  </applications>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("foot", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-layers-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    inis = {}
    for name, rgb in (("red", "ff0000"), ("blue", "0000ff")):
        inis[name] = os.path.join(workdir, f"{name}.ini")
        with open(inis[name], "w") as f:
            # [colors-dark] only: newer foot warns (a yellow bar) about [colors].
            f.write(f"[colors-dark]\nbackground={rgb}\nforeground={rgb}\n")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
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
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    children = []

    def ipc(request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
            c.settimeout(5)
            c.connect(os.path.join(runtime, f"waybox-{sock}.sock"))
            c.sendall((json.dumps(request) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = c.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf)

    def action(name, app_id):
        r = ipc({"request": "action", "action": name, "window": f"app_id:{app_id}"})
        if not r.get("ok"):
            failures.append(f"{name} on {app_id}: {r}")
        time.sleep(0.4)

    def overlap():
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        return subprocess.run(["magick", shot, "-format", "%[pixel:p{350,300}]", "info:"],
                              capture_output=True, text=True).stdout.strip()

    def layers():
        ws = ipc({"request": "get_windows"}).get("windows", [])
        return {w["app_id"]: w.get("layer") for w in ws}

    try:
        for name in ("red", "blue"):
            children.append(subprocess.Popen(["foot", "-c", inis[name], f"--app-id={name}"],
                                             env=cenv, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL))
            time.sleep(1.2)
        # 1. Red always on top (the rule), though blue opened after it.
        if overlap() != RED:
            failures.append(f"red (always on top) under blue: {overlap()}")
        if layers() != {"red": "above", "blue": "normal"}:
            failures.append(f"layers by the rule: {layers()}")
        # 2. Red normal again: blue raised covers it.
        action("SendToNormalLayer", "red")
        action("Raise", "blue")
        if overlap() != BLUE:
            failures.append(f"red back to normal, blue raised: {overlap()}")
        # 3. Blue always on bottom: raised, it stays under red.
        action("ToggleAlwaysOnBottom", "blue")
        action("Raise", "blue")
        if overlap() != RED:
            failures.append(f"blue always on bottom, raised: {overlap()}")
        if layers() != {"red": "normal", "blue": "below"}:
            failures.append(f"layers after the actions: {layers()}")
        # Toggled again: back to normal.
        action("ToggleAlwaysOnBottom", "blue")
        if layers().get("blue") != "normal":
            failures.append(f"ToggleAlwaysOnBottom twice: {layers()}")
    except (OSError, ValueError) as e:
        failures.append(str(e))
    finally:
        for c in children:
            c.kill()
            c.wait()
        time.sleep(0.3)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration window layers: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
