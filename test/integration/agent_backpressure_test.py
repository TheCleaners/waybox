#!/usr/bin/env python3
"""Integration test: the hub's agent socket bounds what one agent can make
it hold (doc/agent-tools.md: MaxQueued, MaxAgents, MaxListens).

A headless Waybox with agent tools on runs the real hub:
1. a slow reader (handshake, then requests, never reading answers): the
   hub stops taking its requests once its answers pass MaxQueued, so the
   sender stalls; the hub's memory stays bounded; another agent (a shim)
   is still answered meanwhile; read at last, every answer arrives
   (paused, never dropped);
2. MaxAgents connections owing their handshake: the next is refused at
   once; once they go, a shim connects again;
3. a stateless agent done sending with a listen stream open: the
   stream's closing result comes, with its id, before the connection
   ends;
4. a label with a newline in it: escaped in the hub's log, so it can't
   forge a line.

Skips (exit 77) without a headless display.

Usage: agent_backpressure_test.py <waybox> <hub> <shim>
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77
MAX_AGENTS = 4
META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {}}


def rss_kib(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


def main():
    waybox, hub, shim = sys.argv[1:4]
    work = tempfile.mkdtemp(prefix="waybox-agentbp-", dir="/tmp")
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    tools = os.path.join(work, "config", "agents", "tools")
    os.makedirs(tools)
    with open(os.path.join(tools, "settings.ini"), "w") as f:
        # (small limits, set as the spec has it, so this is quick)
        f.write("[Agent Tools]\nEnabled=true\n[Tunables]\nMaxQueued=64 KiB\n"
                f"MaxAgents={MAX_AGENTS}\nHandshakeWait=3 s\n")
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=run, XDG_CONFIG_HOME=os.path.join(work, "config"),
               XDG_DATA_HOME=os.path.join(work, "data"), XDG_DATA_DIRS=os.path.join(work, "data-dirs"),
               XDG_CONFIG_DIRS=os.path.join(work, "etc"),
               WB_RC_XML="", WB_MENU_XML="", WAYBOX_AGENT_HUB=hub)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID", "AGENT_TOOLS_SOCKET"):
        env.pop(k, None)
    log_path = os.path.join(work, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    agent_sock = os.path.join(run, "agents", "tools", "agent")
    failures = []

    def text():
        with open(log_path, errors="replace") as f:
            return f.read()

    def wait_for(pred, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.05)
        return pred()

    def connect(handshake=True):
        s = socket.socket(socket.AF_UNIX)
        s.connect(agent_sock)
        if handshake:
            s.sendall(b'{"agent-tools": 1}\n')
            f = s.makefile("rb")
            if not f.readline():
                raise RuntimeError("no handshake answer")
            return s, f
        return s, None

    def shim_answers(timeout=10):
        """A shim's tools/list, answered within `timeout`."""
        try:
            r = subprocess.run([shim], env=env, timeout=timeout, capture_output=True, text=True,
                               input=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list",
                                                 "params": {"_meta": META}}) + "\n")
        except subprocess.TimeoutExpired:
            return False
        return '"tools":[]' in r.stdout

    try:
        if not wait_for(lambda: os.path.exists(agent_sock)):
            if "started the hub" not in text():
                print("SKIP: no hub started (headless display?)", file=sys.stderr)
                return SKIP
            failures.append("the hub's agent socket never appeared")
            return 1
        found = re.findall(r"started the hub \((\d+)\)", text())
        hub_pid = int(found[-1]) if found else 0
        # 1
        slow, slow_f = connect()
        line = (json.dumps({"jsonrpc": "2.0", "id": 0, "method": "ping",
                            "params": {"_meta": META}}) + "\n").encode()
        before = rss_kib(hub_pid) if hub_pid > 1 else None
        slow.setblocking(False)
        payload = line * 15000  # ~2 MB of requests: far past MaxQueued's answers
        sent = 0
        stalled_since = None
        while sent < len(payload):
            try:
                sent += slow.send(payload[sent:sent + 65536])
                stalled_since = None
            except BlockingIOError:
                if stalled_since is None:
                    stalled_since = time.time()
                elif time.time() - stalled_since > 2:
                    break
                time.sleep(0.01)
        if sent >= len(payload):
            failures.append("the hub took every request from an agent reading no answers")
        after = rss_kib(hub_pid) if hub_pid > 1 else None
        if before is not None and after is not None and after - before > 64 * 1024:
            failures.append(f"the hub's memory grew {(after - before) // 1024} MiB for one slow reader")
        if not shim_answers():
            failures.append("another agent wasn't answered while one was paused")
        # Read at last: every request sent so far gets its answer (the one
        # cut short is finished first, in a thread of its own: a hub that
        # never reads again would hold it).
        whole = sent // len(line)
        rest = len(line) - sent % len(line) if sent % len(line) else 0
        target = whole + (1 if rest else 0)
        slow.setblocking(True)
        got = [0]

        def reader():
            while got[0] < target and slow_f.readline():
                got[0] += 1
        t = threading.Thread(target=reader, daemon=True)
        t.start()
        if rest:
            w = threading.Thread(target=slow.sendall, args=(payload[sent:sent + rest],),
                                 daemon=True)
            w.start()
            w.join(timeout=30)
            if w.is_alive():
                failures.append("the hub never read again once the agent caught up")
        t.join(timeout=30)
        if got[0] < target:
            failures.append(f"answers after the pause: {got[0]} of {target}")
        slow_f.close()  # (it holds the socket open too)
        slow.close()
        # 2
        # (wait for each to be counted, well within HandshakeWait, which
        # would close them: counted from what's there before)
        base = text().count(": connected (")
        idle = [connect(handshake=False)[0] for _ in range(MAX_AGENTS)]
        if not wait_for(lambda: text().count(": connected (") >= base + MAX_AGENTS, 2):
            failures.append("the hub didn't take MaxAgents connections")
        extra, _ = connect(handshake=False)
        extra.settimeout(5)
        try:
            refused = extra.recv(1) == b""
        except (socket.timeout, ConnectionResetError):
            refused = False
        if not refused or not wait_for(lambda: "(MaxAgents)" in text()):
            failures.append("a connection past MaxAgents wasn't refused")
        extra.close()
        for s in idle:
            s.close()
        if not wait_for(shim_answers, 15):
            failures.append("no agent could connect once the others went")
        # 3
        s, f = connect()
        s.sendall((json.dumps({"jsonrpc": "2.0", "id": "listen-9", "method": "subscriptions/listen",
                               "params": {"_meta": META,
                                          "notifications": {"toolsListChanged": True}}}) +
                   "\n").encode())
        s.shutdown(socket.SHUT_WR)
        s.settimeout(10)
        msgs = [json.loads(l) for l in f]
        s.close()
        if len(msgs) != 2 or msgs[0].get("method") != "notifications/subscriptions/acknowledged" or \
                msgs[1].get("id") != "listen-9" or "result" not in msgs[1] or \
                msgs[1]["result"]["_meta"].get("io.modelcontextprotocol/subscriptionId") != "listen-9":
            failures.append(f"a listen stream at a graceful end: {msgs}")
        # 4
        subprocess.run([shim, "--label", "x\nwaybox-agent-hub: forged"], env=env, input="",
                       capture_output=True, timeout=10)
        if not wait_for(lambda: "(x\\x0awaybox-agent-hub: forged)" in text()) or \
                "\nwaybox-agent-hub: forged" in text():
            failures.append("an agent's label reached the log unescaped")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        print(text()[-3000:], file=sys.stderr)
        if os.environ.get("WB_KEEP_WORK"):
            print(f"kept: {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
