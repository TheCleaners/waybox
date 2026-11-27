#!/usr/bin/env python3
"""Integration test: Openbox's geometry actions on real windows.

Two undecorated windows (a rule), so a window's frame is its geometry.
1. MoveResizeTo x=10 y=20 width=400 height=300 on "a": there.
2. MoveRelative x=5 y=-10; ResizeRelative right=50 bottom=20: by that much.
3. MoveToCenter: centered on the monitor.
4. "b" placed on the right; "a" moved left of it: GrowToEdge east stops at
   b's left edge; MoveToEdgeNorth (Openbox 3.4's name) at the top.
5. Directional focus: from a, DirectionalTargetWindow east focuses b;
   DirectionalFocusWest (3.4's name) back to a; nothing north: unchanged.
6. ToggleShowDesktop: both minimized; again, both back. On, then a
   focused: the mode ends, a shown, b left minimized.
7. Desktops: AddDesktop adds one at the end; b sent to desktop 2, shown,
   RemoveDesktopCurrent: one fewer, desktop 1 shown, b moved to it; a on
   desktop 2, AddDesktop <where>current</where> from desktop 1: a now on 3.
   Reconfigure keeps the count; rc.xml's number changed, then it wins.
8. b minimized, sent to desktop 2: moved, still minimized. Unfocus leaves
   nothing focused; FocusFallback focuses a again. Kill on b ends its process (SIGTERM).
   (Decorate and the shading ones: window_layers_test, with foot's frame.)
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

        def focused():
            return [w["app_id"] for w in ipc({"request": "get_windows"})["windows"]
                    if w.get("focused")]
        act("Focus", "a")
        expect(focused(), ["a"], "a focused")
        ipc({"request": "action", "action": "DirectionalTargetWindow", "direction": "east"})
        time.sleep(0.4)
        expect(focused(), ["b"], "DirectionalTargetWindow east")
        ipc({"request": "action", "action": "DirectionalFocusWest"})
        time.sleep(0.4)
        expect(focused(), ["a"], "DirectionalFocusWest")
        ipc({"request": "action", "action": "DirectionalTargetWindow", "direction": "north"})
        time.sleep(0.4)
        expect(focused(), ["a"], "nothing north")

        def minimized():
            return sorted(w["app_id"] for w in ipc({"request": "get_windows"})["windows"]
                          if w.get("minimized"))

        def toggle():
            ipc({"request": "action", "action": "ToggleShowDesktop"})
            time.sleep(0.4)
        toggle()
        expect(minimized(), ["a", "b"], "ToggleShowDesktop on")
        toggle()
        expect(minimized(), [], "ToggleShowDesktop off")
        toggle()
        act("Focus", "a")
        expect(minimized(), ["b"], "a focused while the desktop is shown")
        toggle()  # a new show-desktop, not a restore
        expect(minimized(), ["a", "b"], "ToggleShowDesktop after it ended")
        toggle()

        def desktops():
            ds = ipc({"request": "get_desktops"})["desktops"]
            return len(ds), next(d["number"] for d in ds if d.get("current"))

        def on(app_id):
            return next(w["desktop"] for w in ipc({"request": "get_windows"})["windows"]
                        if w["app_id"] == app_id)

        def do(**kw):
            ipc(dict({"request": "action"}, **kw))
            time.sleep(0.3)
        act("Focus", "b")  # still minimized from the show-desktop steps
        n0, _ = desktops()
        do(action="AddDesktop")
        expect(desktops(), (n0 + 1, 1), "AddDesktop")
        do(action="SendToDesktop", to="2", follow=False, window="app_id:b")
        expect(on("b"), 2, "b sent to desktop 2")
        do(action="GoToDesktop", to="2")
        do(action="RemoveDesktopCurrent")
        expect((desktops(), on("b")), ((n0, 1), 1), "RemoveDesktopCurrent")
        do(action="SendToDesktop", to="2", follow=False, window="app_id:a")
        do(action="AddDesktop", where="current")
        expect((desktops(), on("a")), ((n0 + 1, 1), 3), "AddDesktop after the current")
        # Kept across a reload (wbconf's Save & Apply), until rc.xml's
        # number changes.
        do(action="Reconfigure")
        time.sleep(0.4)
        expect(desktops()[0], n0 + 1, "added desktops after a Reconfigure")
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(RC.replace("<applications>",
                               "<desktops><number>2</number></desktops><applications>"))
        do(action="Reconfigure")
        time.sleep(0.4)
        expect(desktops()[0], 2, "rc.xml's new number after a Reconfigure")
        # Names stay with their desktops when one is added or removed
        # between them.
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(RC.replace("<applications>",
                               "<desktops><number>2</number><names><name>Web</name>"
                               "<name>Mail</name></names></desktops><applications>"))
        do(action="Reconfigure")
        time.sleep(0.4)

        def names():
            return [d["name"] for d in ipc({"request": "get_desktops"})["desktops"]]
        do(action="GoToDesktop", to="1")
        do(action="AddDesktop", where="current")
        got = names()
        if len(got) != 3 or got[0] != "Web" or got[2] != "Mail" or got[1] in ("Web", "Mail"):
            failures.append(f"names after AddDesktop current: {got}")
        do(action="GoToDesktop", to="2")
        do(action="RemoveDesktopCurrent")
        expect(names(), ["Web", "Mail"], "names after RemoveDesktopCurrent")
        do(action="GoToDesktop", to="1")
        do(action="SendToDesktop", to="1", follow=False, window="app_id:a")

        # A minimized window is sent too, and stays minimized.
        act("Iconify", "b")
        do(action="SendToDesktop", to="2", follow=False, window="app_id:b")
        expect((on("b"), "b" in minimized()), (2, True), "a minimized window sent")
        act("Focus", "a")
        act("Unfocus", "a")
        expect(focused(), [], "Unfocus")
        ipc({"request": "action", "action": "FocusFallback"})
        time.sleep(0.3)
        expect(focused(), ["a"], "FocusFallback")
        act("Kill", "b")
        for _ in range(20):
            if children[1].poll() is not None:
                break
            time.sleep(0.1)
        expect(children[1].poll() is not None, True, "Kill ended b's process")
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
