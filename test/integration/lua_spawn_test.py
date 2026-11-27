#!/usr/bin/env python3
"""Integration test: programs a Lua script starts (wb.spawn).

init.lua starts three: one that writes nonstop (cat /dev/zero), one that
sleeps quietly, one that ignores SIGTERM. Then:

1. While the writer runs, Waybox answers (IPC round trips stay quick):
   its output is read a bounded amount per turn of the event loop.
2. A Reconfigure loads the script afresh (a new init.lua): the old one's
   programs are stopped, the quiet one at once, the one ignoring SIGTERM
   2 s later; none is left a zombie.

There's no rc.xml: the defaults, the scripting socket among them (on).
Skips (77) when Waybox was built without Lua.

Usage: lua_spawn_test.py <waybox>
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

FIRST = """
wb.spawn({"cat", "/dev/zero"}, function(out, code, truncated)
  wb.log("writer ended truncated=" .. tostring(truncated))
end)
local quiet = wb.spawn({"sleep", "301"}, function() end)
wb.log("spawned quiet " .. quiet)
local stubborn = wb.spawn("trap '' TERM; exec sleep 302", function() end)
wb.log("spawned stubborn " .. stubborn)
wb.log("first loaded")
"""

SECOND = """
wb.log("second loaded")
"""


def state(pid):
    """A process's state letter ("" if it's gone)."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(") ", 1)[1][0]
    except (OSError, IndexError):
        pass
    if os.path.exists("/proc/self/stat"):
        return ""
    r = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True)
    return r.stdout.strip()[:1]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    workdir = tempfile.mkdtemp(prefix="waybox-luaspawn-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    init = os.path.join(cfg, "waybox", "init.lua")
    with open(init, "w") as f:
        f.write(FIRST)
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
    failures = []

    def text():
        log.flush()
        return open(log_path).read()

    sock = None
    for _ in range(80):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        if "first loaded" in text() or "Lua scripting isn't available" in text():
            socks = [n for n in os.listdir(runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks and os.path.exists(os.path.join(runtime, f"waybox-{socks[0]}.sock")):
                sock = socks[0]
                break
    if "first loaded" not in text():
        log.close()
        proc.kill()
        print("SKIP: no Lua (or no headless display)", file=sys.stderr)
        sys.exit(SKIP)
    if sock is None:
        proc.kill()
        log.close()
        print("FAIL: no scripting socket (on by default, with no rc.xml)", file=sys.stderr)
        return 1

    def ipc(request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
            c.settimeout(10)
            c.connect(os.path.join(runtime, f"waybox-{sock}.sock"))
            c.sendall((json.dumps(request) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = c.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf)

    def pid_of(name):
        for line in text().splitlines():
            if f"spawned {name} " in line:
                return int(line.rsplit(" ", 1)[1])
        return None

    try:
        quiet, stubborn = pid_of("quiet"), pid_of("stubborn")
        if quiet is None or stubborn is None:
            failures.append("the script's programs didn't start")
        # 1. Answers while the writer writes.
        time.sleep(0.5)
        slowest = 0.0
        for _ in range(10):
            t0 = time.time()
            ipc({"request": "get_desktops"})
            slowest = max(slowest, time.time() - t0)
            time.sleep(0.05)
        if slowest > 0.5:
            failures.append(f"Waybox answered slowly while a program wrote nonstop "
                            f"({slowest:.2f}s)")
        # The reader's longest turn: a bounded amount (64 KiB) each, not
        # all there is while the program keeps writing.
        read = [x for x in ipc({"request": "get_stalls"}).get("scopes", [])
                if x["name"] == "spawn-read"]
        if not read or read[0]["count"] < 10:
            failures.append(f"the writer's output wasn't read: {read}")
        elif read[0]["max_ms"] > 50:
            failures.append(f"one turn of reading a program's output took "
                            f"{read[0]['max_ms']:.0f} ms")
        else:
            print(f"spawn-read: {read[0]['count']:.0f} turns, longest "
                  f"{read[0]['max_ms']:.1f} ms")
        # 2. Reload: the old script's programs stopped.
        with open(init, "w") as f:
            f.write(SECOND)
        proc.send_signal(signal.SIGUSR2)
        for _ in range(30):
            if "second loaded" in text():
                break
            time.sleep(0.1)
        time.sleep(0.5)
        if quiet is not None and state(quiet) not in ("",):
            failures.append(f"the old script's quiet program wasn't stopped on reload "
                            f"(state {state(quiet)!r})")
        time.sleep(2.5)
        if stubborn is not None and state(stubborn) not in ("",):
            failures.append(f"the old script's program ignoring SIGTERM wasn't killed "
                            f"(state {state(stubborn)!r})")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for p in (pid_of("quiet"), pid_of("stubborn")):
            if p is not None:
                try:
                    os.kill(p, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
        if proc.poll() is None:
            proc.terminate()
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
        sys.stderr.write(t[-2500:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration lua_spawn: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
