#!/usr/bin/env python3
"""Integration test: a layer surface keeping clear of panels (exclusive zone
0) does, whatever the layers.

A 30px bar on the top layer (layer_client: a panel, as Waybar) and a
surface covering the output in the background layer that asks to keep
clear of panels (desktop_client ... avoid, as a dock or desktop widget):
1. Bar first: the background surface is given 1280x690, not the whole
   output (arranged before the bar's reservation, it got 1280x720).
2. Background surface first: when the bar comes, it's given 1280x690.
Skips (exit 77) without a headless display.

Usage: layer_order_test.py <waybox> <layer_client> <desktop_client>
"""
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77


class Client:
    def __init__(self, argv, env):
        self.p = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout or []:
            self.lines.put(line.strip())

    def sizes(self, wait):
        """The sizes given so far, waiting `wait` seconds for more."""
        out = []
        end = time.time() + wait
        while time.time() < end:
            try:
                l = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if l.startswith("size "):
                out.append(l.split()[1])
        return out

    def stop(self):
        self.p.kill()
        self.p.wait()


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, bar_bin, desk_bin = sys.argv[1:]
    work = tempfile.mkdtemp(prefix="waybox-layerorder-", dir="/tmp")
    cfg = os.path.join(work, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    runtime = os.path.join(work, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=work,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(work, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    sock = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        s = [n for n in os.listdir(runtime) if n.startswith("wayland-") and not n.endswith(".lock")]
        if s:
            sock = s[0]
            break
    if sock is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    try:
        # 1. Bar first.
        bar = Client([bar_bin], cenv)
        time.sleep(0.6)
        desk = Client([desk_bin, "background", "avoid"], cenv)
        got = desk.sizes(1.5)
        if not got or got[-1] != "1280x690":
            failures.append(f"bar first: the background surface given {got}, not 1280x690")
        desk.stop()
        bar.stop()
        time.sleep(0.5)
        # 2. The background surface first; then the bar.
        desk = Client([desk_bin, "background", "avoid"], cenv)
        first = desk.sizes(1.0)
        bar = Client([bar_bin], cenv)
        after = desk.sizes(1.5)
        if first[-1:] != ["1280x720"]:
            failures.append(f"alone: given {first}, not 1280x720")
        if after[-1:] != ["1280x690"]:
            failures.append(f"when the bar came: given {after}, not 1280x690")
        desk.stop()
        bar.stop()
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if any(s in open(os.path.join(work, "waybox.log")).read()
           for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration layer order: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
