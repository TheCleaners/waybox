#!/usr/bin/env python3
"""Integration test: Openbox's action names and their directions.

On a 300x200 window, over IPC (any rc.xml action by name):
- MaximizeHorz fills the width only; MaximizeVert then fills the height
  too (fully maximized); UnmaximizeHorz gives the width back only;
  Unmaximize restores it.
- ToggleMaximizeVert toggles the height only, ToggleMaximize both.
- ToggleFullscreen (Openbox's name) toggles fullscreen.
- ToggleShade does nothing to a window without Waybox's frame (nothing
  to roll up to); with a frame (foot, decorated by a rule) it shades and
  unshades.
- An rc.xml keybinding written as Openbox writes it,
  <action name="ToggleMaximize"><direction>vertical</direction></action>,
  maximizes vertically only.
Skips (77) without wtype or a headless display.

Usage: openbox_actions_test.py <waybox> <touch_client>
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-v">
      <action name="ToggleMaximize"><direction>vertical</direction></action>
    </keybind>
  </keyboard>
  <applications>
    <application class="foot"><decor>yes</decor></application>
  </applications>
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
"""


def ipc(runtime, display, request):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
        c.settimeout(5)
        c.connect(os.path.join(runtime, f"waybox-{display}.sock"))
        c.sendall((json.dumps(request) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = c.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-obactions-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def act(name, app="touch-yes"):
        r = ipc(runtime, sock, {"request": "action", "action": name, "window": f"app_id:{app}"})
        if not r.get("ok"):
            failures.append(f"{name}: {r.get('error')}")
        time.sleep(0.4)

    def win():
        for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]:
            if w["app_id"] == "touch-yes":
                return w
        raise RuntimeError("the window went away")

    client = subprocess.Popen([client_bin, "yes"], env=cenv, stdout=subprocess.DEVNULL)
    wbtest.reap_on_exit(client)
    try:
        time.sleep(1.0)
        out = ipc(runtime, sock, {"request": "get_outputs"})["outputs"][0]
        ow, oh = out["width"], out["height"]
        w0 = win()
        if (w0["width"], w0["height"]) != (300, 200):
            raise RuntimeError(f"unexpected start size: {w0}")

        def size():
            w = win()
            return w["width"], w["height"], w["maximized"]

        act("MaximizeHorz")
        w, h, m = size()
        if not (w > ow * 0.9 and h == 200 and not m):
            failures.append(f"MaximizeHorz: {w}x{h} maximized={m}")
        act("MaximizeVert")
        w, h, m = size()
        if not (w > ow * 0.9 and h > oh * 0.8 and m):
            failures.append(f"MaximizeVert after Horz: {w}x{h} maximized={m}")
        act("UnmaximizeHorz")
        w, h, m = size()
        if not (w == 300 and h > oh * 0.8 and not m):
            failures.append(f"UnmaximizeHorz: {w}x{h} maximized={m}")
        act("Unmaximize")
        w, h, m = size()
        if (w, h, m) != (300, 200, False):
            failures.append(f"Unmaximize: {w}x{h} maximized={m}")
        act("ToggleMaximizeVert")
        w, h, m = size()
        if not (w == 300 and h > oh * 0.8):
            failures.append(f"ToggleMaximizeVert on: {w}x{h}")
        act("ToggleMaximizeVert")
        w, h, m = size()
        if (w, h) != (300, 200):
            failures.append(f"ToggleMaximizeVert off: {w}x{h}")
        act("ToggleMaximize")
        if not size()[2]:
            failures.append("ToggleMaximize didn't maximize")
        act("ToggleMaximize")
        if size() != (300, 200, False):
            failures.append(f"ToggleMaximize didn't restore: {size()}")
        act("ToggleFullscreen")
        if not win()["fullscreen"]:
            failures.append("ToggleFullscreen didn't make it fullscreen")
        act("ToggleFullscreen")
        if win()["fullscreen"]:
            failures.append("ToggleFullscreen didn't leave fullscreen")
        act("ToggleShade")
        if win()["shaded"]:
            failures.append("ToggleShade shaded a window without a frame")
        if shutil.which("foot"):
            foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            wbtest.reap_on_exit(foot)
            time.sleep(1.5)

            def foot_shaded():
                for x in ipc(runtime, sock, {"request": "get_windows"})["windows"]:
                    if x["app_id"] == "foot":
                        return x["shaded"]
                raise RuntimeError("foot never showed")
            act("ToggleShade", "foot")
            if not foot_shaded():
                failures.append("ToggleShade didn't shade a framed window")
            act("ToggleShade", "foot")
            if foot_shaded():
                failures.append("ToggleShade didn't unshade it")
            foot.kill()
            time.sleep(0.3)
        # Openbox's rc.xml form: ToggleMaximize with <direction>.
        subprocess.run(["wtype", "-M", "logo", "-k", "v", "-m", "logo"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(0.5)
        w, h, m = size()
        if not (w == 300 and h > oh * 0.8 and not m):
            failures.append(f"<direction>vertical</direction> from rc.xml: {w}x{h} maximized={m}")
    except (RuntimeError, OSError, KeyError) as e:
        failures.append(str(e))
    finally:
        client.kill()
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
    print("integration openbox actions: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
