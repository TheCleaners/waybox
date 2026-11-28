#!/usr/bin/env python3
"""Integration test: Waybox's side of org.freedesktop.AgentCompositor1.

A headless Waybox, agent tools on, starts a stand-in hub
(agent_hub_standin.py) as WAYBOX_AGENT_HUB, handing it the compositor
interface's socket:
1. the hub connects (peer-to-peer D-Bus) and Hello answers version 1,
   Waybox's name, nothing of capture or input yet, not locked;
2. the properties: version 1, Locked, Away and AssistantsStopped false;
3. a member not implemented yet (Capture) answers NotSupported;
4. the session locked (lock_client): Locked and Away change to true;
   unlocked: back to false;
5. SIGTERM: Waybox exits cleanly (0) with its D-Bus thread running, and
   the hub sees the connection close.

Skips (exit 77) without the Python GObject bindings or a headless display.

Usage: agent_compositor_test.py <waybox> <lock_client>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    waybox, lock_client = sys.argv[1:3]
    try:
        import gi
        gi.require_version("Gio", "2.0")
    except (ImportError, ValueError):
        print("SKIP: no Python GObject bindings", file=sys.stderr)
        return SKIP
    work = tempfile.mkdtemp(prefix="waybox-agentcomp-", dir="/tmp")
    out = os.path.join(work, "hub.out")
    open(out, "w").close()
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    tools = os.path.join(work, "config", "agents", "tools")
    os.makedirs(tools)
    with open(os.path.join(tools, "settings.ini"), "w") as f:
        f.write("[Agent Tools]\nEnabled=true\n")
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=run, XDG_CONFIG_HOME=os.path.join(work, "config"),
               WB_RC_XML="", WB_MENU_XML="", STANDIN_OUT=out,
               WAYBOX_AGENT_HUB=f"{sys.executable} {os.path.join(HERE, 'agent_hub_standin.py')}")
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID"):
        env.pop(k, None)
    log = open(os.path.join(work, "waybox.log"), "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    failures = []

    def lines():
        with open(out) as f:
            return [l.rstrip("\n") for l in f]

    def wait_for(pred, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred(lines()):
                return True
            time.sleep(0.05)
        return pred(lines())

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
        # 1-3
        if not wait_for(lambda l: any(x.startswith("capture-error") for x in l)):
            failures.append(f"the hub's first calls: {lines()}")
        got = lines()
        hello = next((x for x in got if x.startswith("hello ")), "")
        if not hello.startswith('hello 1 {') or '"name": "waybox"' not in hello or \
                '"capture": false' not in hello or '"locked": false' not in hello:
            failures.append(f"Hello: {hello}")
        for want in ("prop version 1", "prop Locked False", "prop Away False",
                     "prop AssistantsStopped False",
                     "capture-error org.freedesktop.DBus.Error.NotSupported"):
            if want not in got:
                failures.append(f"missing: {want} ({got})")
        # 4
        lenv = dict(env, WAYLAND_DISPLAY=sock)
        locker = subprocess.Popen([lock_client], env=lenv, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if not wait_for(lambda l: "changed Locked=True" in l and "changed Away=True" in l):
            failures.append(f"locking: {lines()}")
        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        locker.wait(timeout=10)
        if not wait_for(lambda l: "changed Locked=False" in l and "changed Away=False" in l):
            failures.append(f"unlocking: {lines()}")
        # 5
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            failures.append("waybox didn't exit on SIGTERM")
            proc.kill()
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        if not wait_for(lambda l: "closed" in l, 5):
            failures.append(f"the hub didn't see its connection close: {lines()}")
        if any(x.startswith("error") for x in lines()):
            failures.append(f"the hub failed: {lines()}")
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    if failures:
        with open(os.path.join(work, "waybox.log")) as f:
            print(f.read()[-3000:], file=sys.stderr)
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration agent_compositor: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
