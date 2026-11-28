#!/usr/bin/env python3
"""Integration test: a GPU reset where a monitor can't take the new
renderer.

Two headless monitors. A GPU reset (WAYBOX_TEST_GPU_RESET=1, over IPC),
the first monitor's switch to the new renderer made to fail
(WAYBOX_TEST_FAIL=init_render):

1. Waybox goes on: that monitor is turned off (it still points at the old
   renderer, which is kept, not freed under it), the other draws.
2. Turned on again (wlr-randr), it takes the current renderer and draws.
3. Waybox exits cleanly (the old renderer freed then), no sanitizer report.

Skips (exit 77) without grim or wlr-randr, or a headless display.

Usage: render_reset_output_test.py <waybox>
"""
import json
import os
import re
import shutil
import signal
import socket
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
    for tool in ("grim", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-resetout-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2", XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="", WAYBOX_TEST_GPU_RESET="1",
               WAYBOX_TEST_FAIL="init_render",
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
        if socks and os.path.exists(os.path.join(runtime, f"waybox-{socks[0]}.sock")):
            sock = socks[0]
            break
    if sock is None:
        log.close()
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    for k in ("WAYBOX_TEST_GPU_RESET", "WAYBOX_TEST_FAIL"):
        cenv.pop(k, None)
    failures = []

    def text():
        log.flush()
        return open(log_path).read()

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

    def enabled(name):
        out = subprocess.run(["wlr-randr"], env=cenv, capture_output=True, text=True).stdout
        block = out.split(name, 1)[1].split("\nHEADLESS", 1)[0] if name in out else ""
        m = re.search(r"Enabled: (\w+)", block)
        return m.group(1) == "yes" if m else None

    def draws(name):
        shot = os.path.join(workdir, f"{name}.png")
        if os.path.exists(shot):
            os.unlink(shot)
        r = subprocess.run(["grim", "-o", name, shot], env=cenv, capture_output=True,
                           timeout=10)
        return r.returncode == 0 and os.path.exists(shot)

    try:
        time.sleep(0.5)
        # 1. The reset; the first monitor can't switch.
        r = ipc({"request": "test_renderer_lost"})
        if not r.get("ok"):
            failures.append(f"the test GPU reset wasn't taken: {r}")
        time.sleep(1.0)
        if "wb-renderer re-created" not in text():
            failures.append("the renderer wasn't re-created")
        off = re.findall(r"wb-renderer (\S+) can't render with the new renderer", text())
        if len(off) != 1:
            failures.append(f"expected one monitor that couldn't switch: {off}")
        else:
            name = off[0]
            other = "HEADLESS-2" if name == "HEADLESS-1" else "HEADLESS-1"
            if enabled(name) is not False:
                failures.append(f"{name} wasn't turned off: " + subprocess.run(
                    ["wlr-randr"], env=cenv, capture_output=True, text=True).stdout[:600])
            if not draws(other):
                failures.append(f"{other} doesn't draw after the reset")
            # 2. On again: the current renderer, drawing.
            subprocess.run(["wlr-randr", "--output", name, "--on", "--pos", "2000,0"],
                           env=cenv, capture_output=True, timeout=10)
            time.sleep(0.8)
            if f"wb-renderer {name} takes the current renderer" not in text():
                failures.append(f"{name}, turned on, didn't take the current renderer")
            if enabled(name) is not True or not draws(name):
                failures.append(f"{name} doesn't draw once on again")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        # 3. A clean exit.
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox didn't exit")
        log.close()
    t = open(log_path).read()
    if proc.returncode not in (0, None) or any(s in t for s in ("Sanitizer", "runtime error")):
        failures.append(f"waybox ended badly (status {proc.returncode})")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(t[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration render_reset_output: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
