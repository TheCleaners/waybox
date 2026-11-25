#!/usr/bin/env python3
"""Integration test: Openbox's If, ForEach and Stop, from rc.xml keybindings.

Two windows: touch-yes (older), then touch-no (focused).
- W-1  If <class>touch-no</class>: <then> runs (a marker), not <else>.
- W-2  If <maximized>no</maximized> then Maximize, else Unmaximize: twice
       maximizes, then restores.
- W-3  ForEach <class>touch-*</class> Iconify: both windows minimized.
- W-4  ForEach (same) Iconify, Stop: only the first (oldest) minimized.
- W-5  ForEach <class>nothing</class>: <none> runs (a marker).
- W-6  Execute, Stop, Execute: the second Execute doesn't run.
- W-7  If <query target="focus"><class>touch-no</class></query>: true,
       for the focused window, whatever the action's window.
- A right-click on the empty desktop (a mouse binding): Execute, Stop,
  Execute: the second doesn't run.
Skips (77) without wtype, wlrctl or a headless display.

Usage: if_foreach_test.py <waybox> <touch_client>
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


def rc(work):
    def mark(name):
        return (f'<action name="Execute"><execute>touch {os.path.join(work, name)}'
                f'</execute></action>')
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-1"><action name="If"><class>touch-no</class>
      <then>{mark("then-1")}</then><else>{mark("else-1")}</else></action></keybind>
    <keybind key="W-2"><action name="If"><maximized>no</maximized>
      <then><action name="Maximize"/></then><else><action name="Unmaximize"/></else>
    </action></keybind>
    <keybind key="W-3"><action name="ForEach"><class>touch-*</class>
      <then><action name="Iconify"/></then></action></keybind>
    <keybind key="W-4"><action name="ForEach"><class>touch-*</class>
      <then><action name="Iconify"/><action name="Stop"/></then></action></keybind>
    <keybind key="W-5"><action name="ForEach"><class>nothing</class>
      <none>{mark("none-5")}</none></action></keybind>
    <keybind key="W-6">{mark("a-6")}<action name="Stop"/>{mark("b-6")}</keybind>
    <keybind key="W-7"><action name="If">
      <query target="focus"><class>touch-no</class></query>
      <then>{mark("then-7")}</then></action></keybind>
  </keyboard>
  <mouse><context name="Root"><mousebind button="Right" action="Press">
    {mark("c-8")}<action name="Stop"/>{mark("d-8")}
  </mousebind></context></mouse>
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
    for tool in ("wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-iffor-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(rc(workdir))
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
    clients = []

    def key(n):
        subprocess.run(["wtype", "-M", "logo", "-k", str(n), "-m", "logo"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(0.5)

    def wins():
        return {w["app_id"]: w for w in
                ipc(runtime, sock, {"request": "get_windows"})["windows"]}

    def marked(name):
        return os.path.exists(os.path.join(workdir, name))

    def restore_both():
        # Back from minimized, touch-no focused last (as at the start).
        for app in ("touch-yes", "touch-no"):
            ipc(runtime, sock, {"request": "action", "action": "Focus",
                                "window": f"app_id:{app}"})
            time.sleep(0.2)

    try:
        for kind in ("yes", "no"):
            c = subprocess.Popen([client_bin, kind], env=cenv, stdout=subprocess.DEVNULL)
            wbtest.reap_on_exit(c)
            clients.append(c)
            time.sleep(0.7)
        if not wins()["touch-no"]["focused"]:
            raise RuntimeError("touch-no isn't focused at the start")

        key(1)
        if not marked("then-1") or marked("else-1"):
            failures.append(f"If: then={marked('then-1')} else={marked('else-1')}")
        key(2)
        if not wins()["touch-no"]["maximized"]:
            failures.append("If <maximized>no</maximized> didn't maximize")
        key(2)
        if wins()["touch-no"]["maximized"]:
            failures.append("If's <else> didn't unmaximize")
        key(3)
        w = wins()
        if not (w["touch-yes"]["minimized"] and w["touch-no"]["minimized"]):
            failures.append(f"ForEach didn't act on both: "
                            f"{w['touch-yes']['minimized']} {w['touch-no']['minimized']}")
        restore_both()
        key(4)
        w = wins()
        if not w["touch-yes"]["minimized"] or w["touch-no"]["minimized"]:
            failures.append(f"Stop didn't end ForEach after the first (oldest) window: "
                            f"{w['touch-yes']['minimized']} {w['touch-no']['minimized']}")
        restore_both()
        key(5)
        if not marked("none-5"):
            failures.append("ForEach's <none> didn't run")
        key(6)
        if not marked("a-6") or marked("b-6"):
            failures.append(f"Stop in a binding: before={marked('a-6')} after={marked('b-6')}")
        key(7)
        if not marked("then-7"):
            failures.append("a target=\"focus\" query didn't look at the focused window")
        # The windows are at the top left: the far corner is the empty desktop.
        out = ipc(runtime, sock, {"request": "get_outputs"})["outputs"][0]
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv, capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(out["width"] - 20), str(out["height"] - 20)],
                       env=cenv, capture_output=True)
        time.sleep(0.2)
        subprocess.run(["wlrctl", "pointer", "click", "right"], env=cenv, capture_output=True)
        time.sleep(0.5)
        if not marked("c-8") or marked("d-8"):
            failures.append(f"Stop in a mouse binding: before={marked('c-8')} after={marked('d-8')}")
    except (RuntimeError, OSError, KeyError) as e:
        failures.append(str(e))
    finally:
        for c in clients:
            c.kill()
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
    print("integration if/foreach: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
