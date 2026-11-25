#!/usr/bin/env python3
"""Integration test: menus and the on-screen display from Lua.

1. wb.show_menu: an entry's function runs when it's chosen; a nested
   submenu opens; an rc.xml action (Execute) runs; an empty menu isn't
   shown.
2. wb.menu_provider: a menu built afresh each time it opens, as a
   submenu of a script menu and through ShowMenu; a provider that fails
   shows a note instead; a bare <menu id="..."/> in menu.xml finds it,
   with the provider's label.
3. wb.osd shows its text; a menu asked for inside a policy is refused.
4. The windows-menu.lua sample: Super+W lists the windows; choosing the
   one behind focuses it.
Skips (77) when Waybox was built without Lua, or without wtype.

Usage: lua_menu_test.py <waybox> <touch_client> <samples-dir>
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

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-p"><action name="ShowMenu"><menu>lua-dyn</menu></action></keybind>
    <keybind key="W-b"><action name="ShowMenu"><menu>lua-bad</menu></action></keybind>
    <keybind key="W-r"><action name="ShowMenu"><menu>root-menu</menu></action></keybind>
  </keyboard>
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
"""

MENU_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
<menu id="root-menu" label="Waybox">
  <menu id="lua-dyn"/>
</menu>
</openbox_menu>
"""

LUA = r"""
local builds = 0
wb.menu_provider("lua-dyn", function()
  builds = builds + 1
  local n = builds
  return { { label = "Build " .. n, action = function() wb.log("dyn chosen " .. n) end } }
end, "Dynamic")
wb.menu_provider("lua-bad", function() error("boom") end)
wb.bind("W-m", function()
  wb.log("shown " .. tostring(wb.show_menu({
    { label = "First", action = function(w) wb.log("first chosen") end },
    { separator = true, label = "Group" },
    { label = "More", submenu = {
        { label = "Deep", action = function() wb.log("deep chosen") end } } },
    { label = "Provided", submenu = "lua-dyn" },
    { label = "Run", action = { "Execute", command = "touch MARK" } },
  })))
end)
wb.bind("W-e", function() wb.log("empty " .. tostring(wb.show_menu({}))) end)
wb.bind("W-o", function() wb.osd("hello osd", 800) end)
wb.policy("place", function()
  wb.log("menu in policy " .. tostring(wb.show_menu({ { label = "x" } })))
  return nil
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
        f.write(RC_XML)
    with open(os.path.join(cfg, "waybox", "menu.xml"), "w") as f:
        f.write(MENU_XML)
    with open(os.path.join(cfg, "waybox", "init.lua"), "w") as f:
        f.write(lua)
    runtime = os.path.join(workdir, f"run-{tag}")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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



def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, samples = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-luamenu-", dir="/tmp")
    mark = os.path.join(workdir, "mark")
    global LUA
    lua = LUA.replace("touch MARK", f"touch {mark}")

    def steps(cenv, runtime, sock, clients, log, log_path, problems):
        def keys(*names):
            for name in names:
                if name.startswith("W-"):
                    subprocess.run(["wtype", "-M", "logo", "-k", name[2:], "-m", "logo"],
                                   env=cenv, capture_output=True, timeout=10)
                else:
                    subprocess.run(["wtype", "-k", name], env=cenv, capture_output=True,
                                   timeout=10)
                time.sleep(0.3)

        def logged(text):
            log.flush()
            return text in open(log_path).read()

        def count(text):
            log.flush()
            return open(log_path).read().count(text)

        keys("W-m", "Down", "Return")
        if not logged("wb-script: shown true") or not logged("wb-script: first chosen"):
            problems.append("a script menu's entry function didn't run")
        keys("W-m", "Down", "Down", "Right", "Down", "Return")
        if not logged("wb-menu open wb.show_menu/1") or not logged("wb-script: deep chosen"):
            problems.append("a script menu's nested submenu didn't work")
        keys("W-m", "Down", "Down", "Down", "Right", "Down", "Return")
        if not logged("wb-script: dyn chosen 1"):
            problems.append("a provider's menu didn't open as a submenu")
        keys("W-p", "Down", "Return")
        if not logged("wb-script: dyn chosen 2"):
            problems.append("ShowMenu didn't open a provider's menu, built afresh")
        keys("W-r", "Down", "Right", "Down", "Return")
        if not logged("wb-script: dyn chosen 3"):
            problems.append("a bare <menu id> in menu.xml didn't find the provider")
        if not logged("wb-menu label lua-dyn: Dynamic"):
            problems.append("a bare <menu id> didn't take the provider's label")
        keys("W-m", "Down", "Down", "Down", "Down", "Return")
        time.sleep(0.5)
        if not os.path.exists(mark):
            problems.append("a script menu's Execute entry didn't run")
        keys("W-b")
        if not logged("wb-script error in a menu provider") or not logged("boom"):
            problems.append("a failing provider wasn't reported")
        keys("Escape")
        keys("W-e")
        if not logged("wb-script: empty false"):
            problems.append("an empty script menu was shown")
        keys("Escape")
        keys("W-o")
        if not logged("wb-desktop-popup hello osd"):
            problems.append("wb.osd didn't show its text")
        c = Client([client_bin, "yes"], cenv)
        clients.append(c)
        if not c.wait_for("configured"):
            raise RuntimeError("the test window never showed")
        time.sleep(0.3)
        if not logged("wb-script: menu in policy false"):
            problems.append("a menu inside a policy wasn't refused")
        if count("chosen") != 5:
            problems.append(f"entries ran {count('chosen')} times, not 5")

    def sample(cenv, runtime, sock, clients, log, log_path, problems):
        for kind in ("yes", "no"):
            c = Client([client_bin, kind], cenv)
            clients.append(c)
            if not c.wait_for("configured"):
                raise RuntimeError(f"touch-{kind} never showed")
        time.sleep(0.4)
        # Most recently used first: "Desktop 1", touch-no, touch-yes.
        for name in ("W-w", "Down", "Down", "Return"):
            args = ["-M", "logo", "-k", name[2:], "-m", "logo"] if name.startswith("W-") else ["-k", name]
            subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
            time.sleep(0.3)
        time.sleep(0.3)
        ws = {w["app_id"]: w for w in ipc(runtime, sock, {"request": "get_windows"})["windows"]}
        if not ws.get("touch-yes", {}).get("focused"):
            problems.append(f"windows-menu.lua didn't focus the window chosen: {ws}")

    failures = session(waybox, workdir, "menus", lua, steps)
    with open(os.path.join(samples, "windows-menu.lua")) as f:
        failures += session(waybox, workdir, "sample", f.read(), sample)
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lua menus: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
