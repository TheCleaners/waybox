#!/usr/bin/env python3
"""Integration test: waybox-mcp (the shim) and the hub's agent socket.

A headless Waybox with agent tools on runs the real hub; the shim is
driven as an assistant drives it (MCP on its stdin and stdout):
1. no hub (agent tools off): the shim says so and exits non-zero;
2. agent tools on: the shim's handshake (profile safe, its label in the
   hub's log, the agent that started it identified);
3. MCP with initialize (2025-11-25): initialize, an empty tools/list, an
   unknown tool refused;
4. MCP stateless (2026-07-28): server/discover, subscriptions/listen
   acknowledged, tools/list with its cache hints;
5. the agent done sending (stdin closed right after 2000 requests): every
   answer still comes, then the shim exits 0;
6. a token file others can read: refused by the shim; one only the user's:
   sent, and the hub, with no bindings yet, gives "safe" and logs it;
7. straight on the socket: a handshake of another version gets ok false
   and the connection closes; a line longer than MaxFrame closes it;
8. Waybox exits: the shims see the hub go, its socket is removed.

Skips (exit 77) without a headless display.

Usage: agent_shim_test.py <waybox> <hub> <shim>
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


def main():
    waybox, hub, shim = sys.argv[1:4]
    work = tempfile.mkdtemp(prefix="waybox-agentshim-", dir="/tmp")
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    tools = os.path.join(work, "config", "agents", "tools")
    os.makedirs(tools)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=run, XDG_CONFIG_HOME=os.path.join(work, "config"),
               XDG_DATA_HOME=os.path.join(work, "data"), XDG_DATA_DIRS=os.path.join(work, "data-dirs"),
               XDG_CONFIG_DIRS=os.path.join(work, "etc"),
               WB_RC_XML="", WB_MENU_XML="", WAYBOX_AGENT_HUB=hub)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID", "AGENT_TOOLS_SOCKET",
              "AGENT_TOOLS_ASK"):
        env.pop(k, None)
    failures = []
    agent_sock = os.path.join(run, "agents", "tools", "agent")

    # 1
    r = subprocess.run([shim], env=env, input="", capture_output=True, text=True, timeout=10)
    if r.returncode == 0 or "agent tools may be off" not in r.stderr:
        failures.append(f"no hub: {r.returncode} {r.stderr!r}")

    with open(os.path.join(tools, "settings.ini"), "w") as f:
        f.write("[Agent Tools]\nEnabled=true\n")
    log_path = os.path.join(work, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    shims = []

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

    class Shim:
        def __init__(self, *args):
            self.p = subprocess.Popen([shim, *args], env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            shims.append(self.p)

        def send(self, obj):
            self.p.stdin.write(json.dumps(obj) + "\n")
            self.p.stdin.flush()

        def read(self):
            line = self.p.stdout.readline()
            return json.loads(line) if line else None

        def call(self, obj):
            self.send(obj)
            return self.read()

    meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientCapabilities": {},
            "io.modelcontextprotocol/clientInfo": {"name": "stateless-agent", "version": "1"}}

    try:
        if not wait_for(lambda: os.path.exists(agent_sock)):
            if "started the hub" not in text():
                print("SKIP: no hub started (headless display?)", file=sys.stderr)
                return SKIP
            failures.append("the hub's agent socket never appeared")
        # 2, 3
        a = Shim("--label", "laptop")
        init = a.call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                       "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                                  "clientInfo": {"name": "older-agent", "version": "1"}}})
        if not init or init.get("result", {}).get("protocolVersion") != "2025-11-25" or \
                init["result"].get("serverInfo", {}).get("name") != "waybox-agent-hub":
            failures.append(f"initialize: {init}")
        a.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        lst = a.call({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if not lst or lst.get("result", {}).get("tools") != []:
            failures.append(f"tools/list: {lst}")
        call = a.call({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                       "params": {"name": "list_windows", "arguments": {}}})
        if not call or call.get("error", {}).get("code") != -32602:
            failures.append(f"an unknown tool: {call}")
        if not wait_for(lambda: "(laptop): speaks MCP with initialize as \"older-agent\"" in text()):
            failures.append("the hub didn't log the agent's label and MCP era")
        if not wait_for(lambda: "(laptop): profile safe" in text()):
            failures.append("not at safe")
        if "connected (pid " not in text():
            failures.append("the shim's process wasn't identified")
        # (the agent that started it: where /proc says so)
        if sys.platform.startswith("linux") and \
                ", agent " + os.path.realpath(sys.executable) not in text() and \
                ", agent " + sys.executable not in text():
            failures.append("the agent that started the shim wasn't identified")
        # 4
        b = Shim()
        disc = b.call({"jsonrpc": "2.0", "id": "d", "method": "server/discover",
                       "params": {"_meta": meta}})
        res = (disc or {}).get("result", {})
        if res.get("supportedVersions") != ["2026-07-28"] or res.get("resultType") != "complete":
            failures.append(f"server/discover: {disc}")
        ack = b.call({"jsonrpc": "2.0", "id": "listen-1", "method": "subscriptions/listen",
                      "params": {"_meta": meta, "notifications": {"toolsListChanged": True}}})
        if not ack or ack.get("method") != "notifications/subscriptions/acknowledged" or \
                ack["params"]["_meta"].get("io.modelcontextprotocol/subscriptionId") != "listen-1":
            failures.append(f"subscriptions/listen: {ack}")
        lst = b.call({"jsonrpc": "2.0", "id": 5, "method": "tools/list", "params": {"_meta": meta}})
        res = (lst or {}).get("result", {})
        if res.get("tools") != [] or res.get("cacheScope") != "private" or "ttlMs" not in res:
            failures.append(f"stateless tools/list: {lst}")
        # 5
        # (many at once, so answers are still going out when the hub sees
        # the agent is done)
        c = Shim()
        n = 2000
        c.p.stdin.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping",
                                            "params": {"_meta": meta}}) + "\n"
                                for i in range(n)))
        c.p.stdin.close()
        ids = set()
        while True:
            got = c.read()
            if got is None:
                break
            ids.add(got.get("id"))
        try:
            c.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            failures.append("the shim didn't exit after the agent was done")
        if ids != set(range(n)) or c.p.returncode != 0:
            failures.append(f"answers after the agent was done: {len(ids)} of {n}, "
                            f"exit {c.p.returncode}")
        # 6
        tok = os.path.join(work, "token")
        with open(tok, "w") as f:
            f.write("not-a-real-token\n")
        os.chmod(tok, 0o644)
        r = subprocess.run([shim, "--token-file", tok], env=env, input="", capture_output=True,
                           text=True, timeout=10)
        if r.returncode == 0 or "chmod 600" not in r.stderr:
            failures.append(f"a token file others can read: {r.returncode} {r.stderr!r}")
        os.chmod(tok, 0o600)
        r = subprocess.run([shim, "--token-file", tok, "--label", "tokened"], env=env, input="",
                           capture_output=True, text=True, timeout=10)
        if r.returncode != 0 or "profile safe" not in r.stderr:
            failures.append(f"a token file only the user's: {r.returncode} {r.stderr!r}")
        if not wait_for(lambda: "(tokened): profile safe (a token no binding has)" in text()):
            failures.append("the hub didn't log a token no binding has")
        if "not-a-real-token" in text():
            failures.append("the token reached the log")
        # 7
        s = socket.socket(socket.AF_UNIX)
        s.connect(agent_sock)
        s.sendall(b'{"agent-tools": 2}\n')
        s.settimeout(5)
        data = b""
        try:
            while True:
                part = s.recv(4096)
                if not part:
                    break
                data += part
        except socket.timeout:
            failures.append("a refused handshake didn't close the connection")
        s.close()
        ans = json.loads(data.split(b"\n")[0]) if data else {}
        if ans.get("ok") is not False or ans.get("versions") != [1]:
            failures.append(f"a handshake of another version: {data!r}")
        s = socket.socket(socket.AF_UNIX)
        s.connect(agent_sock)
        s.sendall(b'{"agent-tools": 1}\n')
        s.settimeout(5)
        s.recv(4096)
        closed = False
        try:
            s.sendall(b"x" * (5 * 1024 * 1024))
            closed = s.recv(4096) == b""
        except (BrokenPipeError, ConnectionResetError):
            closed = True
        except socket.timeout:
            pass
        s.close()
        if not closed or not wait_for(lambda: "a message longer than MaxFrame" in text()):
            failures.append("a line longer than MaxFrame didn't close the connection")
        # 8
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            failures.append("waybox didn't exit on SIGTERM")
            proc.kill()
        for p in (a.p, b.p):
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                failures.append("a shim didn't notice the hub going")
                continue
            err = p.stderr.read()
            if p.returncode == 0 or "hub closed the connection" not in err:
                failures.append(f"a shim's end: {p.returncode} {err!r}")
        if not wait_for(lambda: not os.path.exists(agent_sock), 3):
            failures.append("the agent socket outlived the hub")
    finally:
        for p in shims:
            if p.poll() is None:
                p.kill()
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
