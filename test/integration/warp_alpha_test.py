#!/usr/bin/env python3
"""Integration test: pointer-warp-v1, alpha-modifier-v1 and wlr-screencopy.

pointer-warp: a window with the pointer in it moves the pointer (it gets
the motion); refused with a wrong serial, to a point outside the window,
and once the pointer has left it.
alpha-modifier: a red window at opacity 0 shows what's behind it; at 1, red.
wlr-screencopy: a capture of the output is its size, and red where that
window is.
Skips (77) without wlrctl, grim, magick or a headless display.

Usage: warp_alpha_test.py <waybox> <warp_alpha_client> <screencopy_client>
"""
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import wbtest

SKIP = 77


class Client:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, text=True, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)
        wbtest.reap_on_exit(self.proc)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def collect(self, seconds):
        out, end = [], time.time() + seconds
        while time.time() < end:
            try:
                out.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return out

    def wait_for(self, prefix, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if line.startswith(prefix):
                return line
        return None

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        got = []
        end = time.time() + 5
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if line == "ok":
                break
            got.append(line)
        return got + self.collect(0.3)


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


def pixel(env, x, y):
    out = subprocess.run(["grim", "-g", f"{x},{y} 1x1", "-t", "ppm", "-"], env=env,
                         capture_output=True, check=True).stdout
    txt = subprocess.run(["magick", "ppm:-", "-format", "%[pixel:p{0,0}]", "info:-"],
                         input=out, capture_output=True, check=True).stdout.decode()
    m = re.search(r"\((\d+),(\d+),(\d+)", txt)
    return tuple(int(v) for v in m.groups()) if m else txt


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, screencopy_bin = sys.argv[1:]
    for tool in ("wlrctl", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-warp-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?><openbox_config xmlns="http://openbox.org/3.4/rc">'
                '<waybox><ipc enabled="yes"/></waybox></openbox_config>\n')
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

    def pointer_to(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=cenv,
                       capture_output=True)
        time.sleep(0.3)

    def window(app_id):
        for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]:
            if w["app_id"] == app_id:
                return w
        return None

    try:
        # pointer-warp.
        c = Client([client_bin, "warp"], cenv)
        if c.wait_for("configured") is None:
            raise RuntimeError("the warp window never showed")
        time.sleep(0.3)
        w = window("warp-test")
        pointer_to(w["x"] + 150, w["y"] + 100)
        if c.wait_for("enter") is None:
            raise RuntimeError("the pointer never entered the window")
        got = c.send("warp 10 20")
        if "motion 10 20" not in got:
            failures.append(f"a warp wasn't honoured: {got}")
        got = c.send("warp-badserial 100 100")
        if any(l.startswith("motion") for l in got):
            failures.append(f"a warp with a wrong serial was honoured: {got}")
        got = c.send("warp 500 500")
        if any(l.startswith("motion") for l in got):
            failures.append(f"a warp outside the window was honoured: {got}")
        out = ipc(runtime, sock, {"request": "get_outputs"})["outputs"][0]
        # Away from it (the far corner, no window there): no longer its to move.
        pointer_to(out["width"] - 5, out["height"] - 5)
        c.collect(0.3)
        got = c.send("warp 30 30")
        if any(l.startswith("motion") for l in got):
            failures.append(f"a window without the pointer moved it: {got}")
        # Only the one honoured warp moved the cursor (a refused one that
        # moved it but sent no motion would pass the checks above).
        log.flush()
        warps = open(log_path).read().count("wb-pointer-warp ")
        if warps != 1:
            failures.append(f"{warps} warps were carried out, not 1")
        c.proc.kill()
        c.proc.wait()
        time.sleep(0.3)

        # alpha-modifier: opacity 0 over the background, then 1.
        bg = pixel(cenv, out["width"] // 2, out["height"] // 2)
        for mult, want_red in (("0", False), ("1", True)):
            a = Client([client_bin, "alpha", mult], cenv)
            if a.wait_for("configured") is None:
                raise RuntimeError(f"the alpha {mult} window never showed")
            time.sleep(0.5)
            w = window("alpha-test")
            px = pixel(cenv, w["x"] + 150, w["y"] + 100)
            red = px == (255, 0, 0)
            if want_red and not red:
                failures.append(f"an opaque window isn't red: {px}")
            if not want_red and px != bg:
                failures.append(f"a window at opacity 0 isn't see-through: {px} (background {bg})")
            if want_red:
                cx, cy = w["x"] + 150, w["y"] + 100
                r = subprocess.run([screencopy_bin, str(cx), str(cy)], env=cenv,
                                   capture_output=True, text=True, timeout=10)
                lines = r.stdout.split("\n")
                if f"size {out['width']} {out['height']}" not in lines:
                    failures.append(f"screencopy: wrong size or no capture: {r.stdout!r}")
                if f"pixel {cx} {cy} 255 0 0" not in lines:
                    failures.append(f"screencopy: not red where the window is: {r.stdout!r}")
            a.proc.kill()
            a.proc.wait()
            time.sleep(0.3)
    except (RuntimeError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
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
    print("integration warp alpha: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
