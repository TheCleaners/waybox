#!/usr/bin/env python3
"""Integration test: waybox-agent-hub, started and supervised by Waybox.

A headless Waybox with the real hub (WAYBOX_AGENT_HUB names the built one):
1. agent tools off (no settings.ini): no hub is started;
2. turned on (Enabled=true) and reconfigured: the hub starts, connects
   (Hello: Waybox), reads the state, and makes its runtime folder, only the
   user's;
3. the hub follows the state: the session locked, then unlocked;
4. the hub killed: restarted after a second, and connects again; killed
   twice more within the minute: not restarted again (given up);
5. reconfigured: a hub given up on gets another go;
6. turned off and reconfigured: the hub ends;
7. SIGTERM, turned on again first: Waybox exits cleanly, the hub with it.

Skips (exit 77) without a headless display.

Usage: agent_hub_test.py <waybox> <hub> <lock_client>
"""
import json
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77


def main():
    waybox, hub, lock_client = sys.argv[1:4]
    work = tempfile.mkdtemp(prefix="waybox-agenthub-", dir="/tmp")
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    cfg = os.path.join(work, "config", "waybox")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"><waybox><ipc enabled="yes"/>'
                '</waybox></openbox_config>\n')
    tools = os.path.join(work, "config", "agents", "tools")
    os.makedirs(tools)
    settings = os.path.join(tools, "settings.ini")
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=run, XDG_CONFIG_HOME=os.path.join(work, "config"),
               XDG_CONFIG_DIRS=os.path.join(work, "etc"),
               WB_RC_XML="", WB_MENU_XML="", WAYBOX_AGENT_HUB=hub)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID"):
        env.pop(k, None)
    log_path = os.path.join(work, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    failures = []

    def text():
        with open(log_path, errors="replace") as f:
            return f.read()

    def count(s):
        return text().count(s)

    def wait_for(pred, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.05)
        return pred()

    def hub_pid():
        found = re.findall(r"agent tools: started the hub \((\d+)\)", text())
        pid = int(found[-1]) if found else 0
        # Never 0 or below: os.kill would reach a whole group, or everything.
        if pid <= 1:
            raise RuntimeError(f"no hub started: {text()[-2000:]}")
        return pid

    def alive(pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        # (a zombie: not reaped yet)
        try:
            with open(f"/proc/{pid}/stat") as f:
                return f.read().split(") ")[-1][0] != "Z"
        except OSError:
            return True

    def turn(on):
        with open(settings, "w") as f:
            f.write(f"[Agent Tools]\nEnabled={'true' if on else 'false'}\n")

    try:
        sock = None
        for _ in range(100):
            time.sleep(0.05)
            s = [n for n in os.listdir(run) if n.startswith("wayland-") and not n.endswith(".lock")]
            if s:
                sock = s[0]
                break
        if sock is None:
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            return SKIP
        ipc_path = os.path.join(run, f"waybox-{sock}.sock")
        wait_for(lambda: os.path.exists(ipc_path))

        def ipc(req):
            c = socket.socket(socket.AF_UNIX)
            c.connect(ipc_path)
            c.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                part = c.recv(65536)
                if not part:
                    break
                data += part
            c.close()
            return json.loads(data)

        # 1
        time.sleep(0.5)
        if "started the hub" in text():
            failures.append("a hub started with agent tools off")
        # 2
        turn(True)
        ipc({"request": "reconfigure"})
        if not wait_for(lambda: "waybox-agent-hub: state locked=false away=false stopped=false"
                        in text()):
            failures.append("the hub didn't start, connect and read the state")
        if "waybox-agent-hub: connected to waybox (compositor interface 1)" not in text():
            failures.append("the hub's Hello")
        rdir = os.path.join(run, "agents", "tools")
        if not os.path.isdir(rdir) or stat.S_IMODE(os.stat(rdir).st_mode) != 0o700 or \
                stat.S_IMODE(os.stat(os.path.join(run, "agents")).st_mode) != 0o700:
            failures.append("the hub's runtime folder isn't only the user's")
        # 3
        locker = subprocess.Popen([lock_client], env=dict(env, WAYLAND_DISPLAY=sock),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        if not wait_for(lambda: "waybox-agent-hub: changed Locked=true" in text()):
            failures.append("the hub didn't hear of the lock")
        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        locker.wait(timeout=10)
        if not wait_for(lambda: "waybox-agent-hub: changed Locked=false" in text()):
            failures.append("the hub didn't hear of the unlock")
        # 4
        first = hub_pid()
        os.kill(first, signal.SIGKILL)
        if not wait_for(lambda: "restarting the hub in 1000 ms" in text()):
            failures.append("no restart after a crash")
        if not wait_for(lambda: count("waybox-agent-hub: connected to waybox") == 2, 5):
            failures.append("the restarted hub didn't connect")
        second = hub_pid()
        if second == first:
            failures.append("no new hub")
        os.kill(second, signal.SIGKILL)
        if not wait_for(lambda: "restarting the hub in 2000 ms" in text()):
            failures.append("the second delay isn't longer")
        if not wait_for(lambda: count("waybox-agent-hub: connected to waybox") == 3, 6):
            failures.append("the hub didn't come back a second time")
        os.kill(hub_pid(), signal.SIGKILL)
        if not wait_for(lambda: "not restarting it until agent tools are reconfigured" in text()):
            failures.append("not given up after three crashes in a minute")
        time.sleep(1.5)
        if count("started the hub") != 3:
            failures.append(f"restarted after giving up: {count('started the hub')} starts")
        # 5
        ipc({"request": "reconfigure"})
        if not wait_for(lambda: count("waybox-agent-hub: connected to waybox") == 4):
            failures.append("reconfiguring didn't give the hub another go")
        # 6
        last = hub_pid()
        turn(False)
        ipc({"request": "reconfigure"})
        if not wait_for(lambda: "agent tools: turned off" in text() and not alive(last)):
            failures.append("turning agent tools off didn't end the hub")
        if "restarting the hub" in text().split("agent tools: turned off")[-1]:
            failures.append("a hub ended on purpose was restarted")
        # 7
        turn(True)
        ipc({"request": "reconfigure"})
        wait_for(lambda: count("waybox-agent-hub: connected to waybox") == 5)
        last = hub_pid()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            failures.append("waybox didn't exit on SIGTERM")
            proc.kill()
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        if not wait_for(lambda: not alive(last), 3):
            failures.append("the hub outlived Waybox")
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        print(text()[-4000:], file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
