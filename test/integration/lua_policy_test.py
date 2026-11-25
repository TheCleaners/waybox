#!/usr/bin/env python3
"""Integration test: Lua policy hooks (wb.policy).

1. "rules" sends one program to desktop 2; "place" puts another at an exact
   spot before its first frame; an action asked for inside a policy is
   refused.
2. "layout": two windows side by side, each half the output; one closed,
   the other takes all of it.
3. The tiling.lua sample: Super+Alt+T tiles the desktop (the focused
   window on the left, the other on the right), and again lets it float.
Skips (77) when Waybox was built without Lua.

Usage: lua_policy_test.py <waybox> <touch_client> <samples-dir>
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

SKIP = 77

RULES_PLACE = r"""
wb.policy("rules", function(w)
  if w.app_id == "touch-yes" then return { desktop = 2 } end
end)
wb.policy("place", function(w, area, proposed)
  if w.app_id ~= "touch-no" then return nil end
  wb.log("action in policy " .. tostring(wb.action("Execute", { command = "true" })))
  return { x = area.x + 33, y = area.y + 44 }
end)
"""

LAYOUT = r"""
wb.policy("layout", function(windows, area, output, desktop)
  local out, n = {}, #windows
  if n == 0 then return out end
  local w = math.floor(area.width / n)
  for i, win in ipairs(windows) do
    out[win.id] = { x = area.x + (i - 1) * w, y = area.y, width = w, height = area.height }
  end
  wb.log("layout " .. n .. " on " .. output .. " desktop " .. desktop)
  return out
end)
"""


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





def session(waybox, workdir, tag, lua, steps):
    cfg = os.path.join(workdir, f"config-{tag}")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
""")
    with open(os.path.join(cfg, "waybox", "init.lua"), "w") as f:
        f.write(lua)
    runtime = os.path.join(workdir, f"run-{tag}")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, f"waybox-{tag}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        proc.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    problems = []
    clients = []
    try:
        log.flush()
        if "wb-scripts loaded" not in open(log_path).read():
            print("SKIP: Waybox was built without Lua", file=sys.stderr)
            proc.kill()
            sys.exit(SKIP)
        steps(dict(env, WAYLAND_DISPLAY=sock), runtime, sock, clients, log, log_path, problems)
    except (RuntimeError, OSError, KeyError, IndexError) as e:
        problems.append(str(e))
    finally:
        for c in clients:
            if c.proc.poll() is None:
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
        problems.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        problems.append("sanitizer/assert output")
    return [f"{tag}: {p}" for p in problems]


def windows(runtime, sock):
    return {w["app_id"]: w for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]}


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, samples = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-luapolicy-", dir="/tmp")

    def rules_place(cenv, runtime, sock, clients, log, log_path, problems):
        for kind in ("yes", "no"):
            c = Client([client_bin, kind], cenv)
            clients.append(c)
            if not c.wait_for("configured"):
                raise RuntimeError(f"touch-{kind} never showed")
        time.sleep(0.5)
        ws = windows(runtime, sock)
        if ws["touch-yes"]["desktop"] != 2:
            problems.append(f"the rules policy didn't send it to desktop 2: {ws['touch-yes']['desktop']}")
        log.flush()
        m = re.findall(r"wb-geom mapped \d+x\d+\+(-?\d+)\+(-?\d+) touch-no", open(log_path).read())
        if not m or tuple(map(int, m[0])) != (33, 44):
            problems.append(f"the place policy didn't place it before it showed: {m[:1]}")
        if "wb-script: action in policy false" not in open(log_path).read():
            problems.append("an action inside a policy wasn't refused")

    def layout(cenv, runtime, sock, clients, log, log_path, problems):
        out = ipc(runtime, sock, {"request": "get_outputs"})["outputs"][0]
        ow, oh = out["width"], out["height"]
        cs = {}
        for kind in ("yes", "no"):
            c = Client([client_bin, kind], cenv)
            clients.append(c)
            cs[kind] = c
            if not c.wait_for("configured"):
                raise RuntimeError(f"touch-{kind} never showed")
        time.sleep(0.6)
        ws = windows(runtime, sock)
        boxes = sorted((w["x"], w["y"], w["width"], w["height"]) for w in ws.values())
        half = ow // 2
        if boxes != [(0, 0, half, oh), (half, 0, half, oh)]:
            problems.append(f"two windows weren't laid out side by side: {boxes}")
        cs["yes"].proc.kill()
        cs["yes"].proc.wait()
        time.sleep(0.6)
        ws = windows(runtime, sock)
        w = ws.get("touch-no")
        if w is None or (w["x"], w["y"], w["width"], w["height"]) != (0, 0, ow, oh):
            problems.append(f"the remaining window didn't take the output: {w}")

    def tiling(cenv, runtime, sock, clients, log, log_path, problems):
        if shutil.which("wtype") is None:
            return
        out = ipc(runtime, sock, {"request": "get_outputs"})["outputs"][0]
        ow, oh = out["width"], out["height"]
        for kind in ("yes", "no"):
            c = Client([client_bin, kind], cenv)
            clients.append(c)
            if not c.wait_for("configured"):
                raise RuntimeError(f"touch-{kind} never showed")
        time.sleep(0.5)
        before = {k: (w["x"], w["y"], w["width"], w["height"])
                  for k, w in windows(runtime, sock).items()}
        subprocess.run(["wtype", "-M", "logo", "-M", "alt", "-k", "t", "-m", "alt", "-m", "logo"],
                       env=cenv, capture_output=True, timeout=10)
        time.sleep(0.6)
        ws = windows(runtime, sock)
        gap, share = 6, 0.55
        w = ow - 2 * gap
        mw = int(w * share)
        main = (gap, gap, mw - gap, oh - 2 * gap)
        stack = (gap + mw, gap, w - mw, oh - 2 * gap)
        got = {k: (v["x"], v["y"], v["width"], v["height"]) for k, v in ws.items()}
        # touch-no opened last: focused, so the main window.
        if got.get("touch-no") != main or got.get("touch-yes") != stack:
            problems.append(f"tiling.lua didn't tile: {got} (main {main}, stack {stack})")
        subprocess.run(["wtype", "-M", "logo", "-M", "alt", "-k", "t", "-m", "alt", "-m", "logo"],
                       env=cenv, capture_output=True, timeout=10)
        time.sleep(0.4)
        log.flush()
        if "tiling on desktop 1: off" not in open(log_path).read():
            problems.append("tiling.lua didn't turn off again")

    failures = session(waybox, workdir, "rules", RULES_PLACE, rules_place)
    failures += session(waybox, workdir, "layout", LAYOUT, layout)
    with open(os.path.join(samples, "tiling.lua")) as f:
        failures += session(waybox, workdir, "tiling", f.read(), tiling)
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lua policy: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
