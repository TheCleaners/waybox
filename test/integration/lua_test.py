#!/usr/bin/env python3
"""Integration test: Lua scripts (init.lua).

A script that listens to events, binds keys, sets timers, misbehaves on
purpose, and checks its sandbox:
- window.new and window.close arrive (a closed window's handle reads nil);
- desktop.switch arrives;
- a bound key runs with the focused window, whose action works (maximize);
- a one-shot timer fires; a repeating one cancels itself after 3 runs;
- an error is logged with the file and line, and Waybox goes on;
- an endless loop is stopped by the time budget, and after 3 times the
  binding is turned off; Waybox stays responsive;
- the whole standard library is there (io, os, require);
- a Reconfigure loads the scripts afresh.
Skips (77) when Waybox was built without Lua, or without wtype.

Usage: lua_test.py <waybox> <touch_client>
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

INIT_LUA = r"""
wb.log("stdlib io=" .. type(io) .. " os.execute=" .. type(os.execute) .. " require=" .. type(require))
wb.on("window.new", function(w, info)
  wb.log("new " .. info.app_id .. " handle=" .. tostring(w.app_id))
end)
wb.on("window.close", function(w, info)
  wb.log("close " .. info.app_id .. " gone=" .. tostring(w.app_id == nil))
end)
wb.on("desktop.switch", function(d) wb.log("desktop " .. d) end)
wb.bind("W-y", function(w)
  wb.log("bind " .. (w and w.app_id or "none"))
  if w then w:toggle_maximize() end
end)
wb.bind("W-e", function() error("boom") end)
wb.bind("W-l", function() while true do end end)
wb.timer(150, function() wb.log("timer once") end)
local n, id = 0, nil
id = wb.every(50, function()
  n = n + 1
  if n == 3 then wb.cancel(id); wb.log("every done " .. n) end
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





def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-lua-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <waybox><ipc enabled="yes"/><scripts budget="20"/></waybox>
</openbox_config>
""")
    with open(os.path.join(cfg, "waybox", "init.lua"), "w") as f:
        f.write(INIT_LUA)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
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

    def logtext():
        log.flush()
        return open(log_path).read()

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
        time.sleep(0.3)

    try:
        time.sleep(0.5)
        text = logtext()
        if "wb-scripts loaded" not in text:
            print("SKIP: Waybox was built without Lua", file=sys.stderr)
            proc.kill()
            sys.exit(SKIP)
        if "wb-script: stdlib io=table os.execute=function require=function" not in text:
            failures.append("the whole standard library isn't there")
        if "wb-script: timer once" not in text:
            failures.append("the one-shot timer didn't fire")
        if "wb-script: every done 3" not in text or text.count("every done") != 1:
            failures.append("the repeating timer didn't stop itself after 3 runs")

        win = Client([client_bin, "no"], cenv)
        clients.append(win)
        if not win.wait_for("configured"):
            raise RuntimeError("the window never showed")
        time.sleep(0.5)
        if "wb-script: new touch-no handle=touch-no" not in logtext():
            failures.append("window.new didn't arrive with a live handle")

        keys("-M", "logo", "-k", "y", "-m", "logo")
        time.sleep(0.3)
        w = [x for x in ipc(runtime, sock, {"request": "get_windows"})["windows"]
             if x.get("app_id") == "touch-no"][0]
        if "wb-script: bind touch-no" not in logtext() or not w["maximized"]:
            failures.append("the bound key didn't run with the focused window, or its "
                            "action didn't work")

        keys("-M", "logo", "-k", "e", "-m", "logo")
        if not re.search(r"wb-script error in a key binding: \S*init\.lua:\d+: boom", logtext()):
            failures.append("a script error wasn't logged with its file and line")

        for _ in range(3):
            t0 = time.time()
            keys("-M", "logo", "-k", "l", "-m", "logo")
        text = logtext()
        if text.count("took longer than its time budget") != 3:
            failures.append(f"the endless loop wasn't stopped by the budget "
                            f"({text.count('took longer than its time budget')} times)")
        if "turned off: it kept running too long" not in text:
            failures.append("the looping binding wasn't turned off")
        r = ipc(runtime, sock, {"request": "get_version"})
        if not r.get("ok"):
            failures.append("Waybox stopped answering after the endless loop")

        r = ipc(runtime, sock, {"request": "action", "action": "GoToDesktop", "to": "2"})
        time.sleep(0.3)
        if "wb-script: desktop 2" not in logtext():
            failures.append("desktop.switch didn't arrive")
        ipc(runtime, sock, {"request": "action", "action": "GoToDesktop", "to": "1"})

        win.proc.kill()
        win.proc.wait()
        time.sleep(0.5)
        if "wb-script: close touch-no gone=true" not in logtext():
            failures.append("window.close didn't arrive with a handle that reads nil")

        n_loaded = logtext().count("wb-scripts loaded")
        ipc(runtime, sock, {"request": "action", "action": "Reconfigure"})
        time.sleep(0.5)
        if logtext().count("wb-scripts loaded") != n_loaded + 1:
            failures.append("a Reconfigure didn't load the scripts afresh")
    except (RuntimeError, OSError, KeyError, IndexError) as e:
        failures.append(str(e))
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
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"log: {log_path}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lua: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
