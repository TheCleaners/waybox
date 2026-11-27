#!/usr/bin/env python3
"""Integration test: Openbox's geometry actions on real windows.

Two undecorated windows (a rule), so a window's frame is its geometry.
1. MoveResizeTo x=10 y=20 width=400 height=300 on "a": there.
2. MoveRelative x=5 y=-10; ResizeRelative right=50 bottom=20: by that much.
3. MoveToCenter: centered on the monitor.
4. "b" placed on the right; "a" moved left of it: GrowToEdge east stops at
   b's left edge; MoveToEdgeNorth (Openbox 3.4's name) at the top.
Skips (exit 77) without a headless display.

Usage: geometry_actions_test.py <waybox> <activate_client>
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

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications><application class="*"><decor>no</decor></application></applications>
</openbox_config>
"""


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-geomact-", dir="/tmp")
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

    def geom(app_id):
        for w in ipc({"request": "get_windows"}).get("windows", []):
            if w["app_id"] == app_id:
                return (w["x"], w["y"], w["width"], w["height"])
        return None

    def act(name, app_id, **kw):
        r = ipc(dict({"request": "action", "action": name, "window": f"app_id:{app_id}"}, **kw))
        if not r.get("ok"):
            failures.append(f"{name}: {r}")
        time.sleep(0.4)
        return geom(app_id)

    def window(app_id):
        p = subprocess.Popen([client, app_id], env=cenv, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        children.append(p)
        if p.stdout.readline().strip() != "configured":
            raise RuntimeError(f"{app_id} didn't show")
        time.sleep(0.4)

    def expect(got, want, what):
        if got != want:
            failures.append(f"{what}: {got}, not {want}")

    try:
        out = next(o for o in ipc({"request": "get_outputs"})["outputs"])
        ow, oh = out["width"], out["height"]
        window("a")
        expect(act("MoveResizeTo", "a", x=10, y=20, width=400, height=300), (10, 20, 400, 300),
               "MoveResizeTo")
        expect(act("MoveRelative", "a", x=5, y=-10), (15, 10, 400, 300), "MoveRelative")
        expect(act("ResizeRelative", "a", right=50, bottom=20), (15, 10, 450, 320),
               "ResizeRelative")
        expect(act("MoveToCenter", "a"), ((ow - 450) // 2, (oh - 320) // 2, 450, 320),
               "MoveToCenter")
        window("b")
        act("MoveResizeTo", "b", x=700, y=100, width=200, height=400)
        act("MoveResizeTo", "a", x=50, y=150, width=300, height=200)
        expect(act("GrowToEdge", "a", direction="east"), (50, 150, 650, 200),
               "GrowToEdge east, to b")
        expect(act("MoveToEdgeNorth", "a"), (50, 0, 650, 200), "MoveToEdgeNorth")
    except (OSError, RuntimeError, ValueError, StopIteration, KeyError) as e:
        failures.append(f"{type(e).__name__}: {e}")
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
    print("integration geometry actions: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
