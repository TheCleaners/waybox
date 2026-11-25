#!/usr/bin/env python3
"""Integration test: measuring what blocks the event loop.

With WAYBOX_STALL_MS=0 every timed piece of work counts as slow, and with
WAYBOX_STALL_WATCH=1 the loop watchdog runs. Frames, a Reconfigure (over
IPC) and the watchdog's ticks then show up in the log ("wb-stall ...")
and in get_stalls, which reports counts and times per name.

Usage: stalls_test.py <waybox>
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

SKIP = 77


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
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    workdir = tempfile.mkdtemp(prefix="waybox-stalls-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><ipc enabled="yes"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
               WAYBOX_STALL_MS="0", WAYBOX_STALL_WATCH="1", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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
    failures = []
    try:
        r = ipc(runtime, sock, {"request": "action", "action": "Reconfigure"})
        if not r.get("ok"):
            failures.append(f"Reconfigure over IPC failed: {r}")
        time.sleep(0.6)
        st = ipc(runtime, sock, {"request": "get_stalls"})
        scopes = {s["name"]: s for s in st.get("scopes", [])}
        if st.get("threshold_ms") != 0:
            failures.append(f"WAYBOX_STALL_MS wasn't used: {st.get('threshold_ms')}")
        for name in ("frame", "reconfigure", "loop"):
            s = scopes.get(name)
            if s is None or s["count"] < 1:
                failures.append(f"no {name} timing in get_stalls: {sorted(scopes)}")
            elif s["max_ms"] < 0 or s["mean_ms"] > s["max_ms"] + 1e-6:
                failures.append(f"odd numbers for {name}: {s}")
        log.flush()
        text = open(log_path).read()
        for want in ("wb-stall reconfigure", "wb-stall frame", "wb-stall loop",
                     "Event loop watchdog on"):
            if want not in text:
                failures.append(f"{want!r} not in the log")
    except (OSError, ValueError, KeyError) as e:
        failures.append(str(e))
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
    print("integration stalls: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
