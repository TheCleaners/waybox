#!/usr/bin/env python3
"""Integration test: a window icon given as pixels (xdg-toplevel-icon's
buffer path, not a name).

A window "pixicon" (no icon of that name anywhere) sets a 32 px magenta
buffer as its icon: the titlebar's left end (<titleLayout>NLIMC) shows
magenta. A second one sets a cyan buffer: its titlebar shows cyan (each
its own icon, not the first's).

Skips (exit 77) without grim, a headless display, or the protocol.

Usage: pixel_icon_test.py <waybox> <icon_buffer_client>
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
  <theme><titleLayout>NLIMC</titleLayout></theme>
  <applications>
    <application class="pixicon"><position><x>100</x><y>100</y></position></application>
    <application class="pixicon2"><position><x>500</x><y>300</y></position></application>
  </applications>
</openbox_config>
"""


def ppm_pixels(data):
    """The (r, g, b) pixels of a binary PPM (P6, 8-bit)."""
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    body = data[i:i + w * h * 3]
    return [tuple(body[k:k + 3]) for k in range(0, len(body), 3)]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client = sys.argv[1:]
    if shutil.which("grim") is None:
        print("SKIP: grim not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-pixicon-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_HOME=os.path.join(workdir, "data"),
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
    failures = []
    kids = []

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

    def window(app_id):
        for w in ipc({"request": "get_windows"}).get("windows", []):
            if w["app_id"] == app_id:
                return w
        return {}

    def colour_count(app_id, colour):
        """Pixels of `colour` in the window's titlebar, left 60 px."""
        w = window(app_id)
        if not w:
            return -1
        x, y = int(w["x"]), int(w["y"])
        r = subprocess.run(["grim", "-t", "ppm", "-g", f"{x},{max(y - 40, 0)} 60x40", "-"],
                           env=cenv, capture_output=True, timeout=10)
        if r.returncode != 0:
            return -1
        return sum(1 for p in ppm_pixels(r.stdout) if p == colour)

    try:
        for app, argb, rgb in (("pixicon", "ffff00ff", (255, 0, 255)),
                               ("pixicon2", "ff00ffff", (0, 255, 255))):
            c = subprocess.Popen([client, app, argb], env=cenv, stdout=subprocess.PIPE,
                                 text=True)
            kids.append(c)
            line = c.stdout.readline().strip()
            if line == "no-icon-protocol":
                print("SKIP: no xdg-toplevel-icon", file=sys.stderr)
                return SKIP
            if line != "configured":
                failures.append(f"{app} didn't show: {line!r}")
                continue
            time.sleep(0.6)
            n = colour_count(app, rgb)
            if n < 100:
                failures.append(f"{app}'s pixel icon isn't in its titlebar ({n} pixels of it)")
        # Each its own: the first still magenta, no cyan.
        if colour_count("pixicon", (0, 255, 255)) > 0:
            failures.append("the first window shows the second's icon")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for k in kids:
            if k.poll() is None:
                k.kill()
            k.wait()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    t = open(log_path).read()
    if any(s in t for s in ("Sanitizer", "runtime error")):
        failures.append("sanitizer output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(t[-2000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration pixel_icon: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
