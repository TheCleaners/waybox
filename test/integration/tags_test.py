#!/usr/bin/env python3
"""Integration test: window tags (xdg-toplevel-tag).

A rule matching tag="player" opens the window on desktop 2. A window
tagged "player" gets it, one tagged "other" doesn't; IPC reports each
window's tag. Skips (77) where the protocol isn't offered (wlroots 0.19).

Usage: tags_test.py <waybox> <tag_client>
"""
import json
import os
import queue
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77


class Client:
    def __init__(self, argv, env, stdin=False):
        self.proc = subprocess.Popen(argv, env=env, text=True,
                                     stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                     stdout=subprocess.PIPE)
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def drain(self, seconds=0.3):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.seen.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return self.seen

    def wait_for(self, want, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.seen.append(line)
            if line == want:
                return True
        return False

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        if not self.wait_for("ok"):
            raise RuntimeError(f"the virtual pointer didn't answer {cmd!r}")



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
    workdir = tempfile.mkdtemp(prefix="waybox-tags-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <applications>
    <application class="tagtest" tag="play*"><desktop>2</desktop></application>
  </applications>
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
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
    failures = []
    clients = []
    skip = False
    try:
        player = Client([client_bin, "player"], cenv)
        clients.append(player)
        if not player.wait_for("configured"):
            if "no-tag-manager" in player.seen:
                skip = True
                raise RuntimeError("skip")
            raise RuntimeError(f"the tagged window never showed: {player.seen}")
        other = Client([client_bin, "other"], cenv)
        clients.append(other)
        if not other.wait_for("configured"):
            raise RuntimeError("the other window never showed")
        time.sleep(0.5)
        wins = {w["tag"]: w for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]
                if w.get("app_id") == "tagtest"}
        if set(wins) != {"player", "other"}:
            failures.append(f"IPC doesn't report the tags: {list(wins)}")
        else:
            if wins["player"]["desktop"] != 2:
                failures.append(f"the tag rule didn't apply: desktop {wins['player']['desktop']}")
            if wins["other"]["desktop"] != 1:
                failures.append(f"the tag rule matched another tag: desktop {wins['other']['desktop']}")
    except RuntimeError as e:
        if not skip:
            failures.append(str(e))
    finally:
        for c in clients:
            c.proc.kill()
            c.proc.wait()
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    if skip:
        print("SKIP: xdg-toplevel-tag not offered (wlroots 0.19)", file=sys.stderr)
        return SKIP
    print("integration tags: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
