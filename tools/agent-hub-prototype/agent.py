#!/usr/bin/env python3
"""A scripted MCP agent: starts the shim, initializes, lists tools, and
runs the calls given on stdin, one JSON object per line:
  {"tool": "ui.ask", "args": {...}}
Prints each result as "result <tool>: <text>".

  agent.py [--profile NAME] < calls.jsonl
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    shim = subprocess.Popen([sys.executable, os.path.join(HERE, "shim.py")] + sys.argv[1:],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    ids = iter(range(1, 10**6))

    def rpc(method, params=None):
        rid = next(ids)
        shim.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                                     "params": params or {}}) + "\n")
        shim.stdin.flush()
        while True:
            line = shim.stdout.readline()
            if not line:
                raise SystemExit("hub went away")
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg

    init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": os.environ.get("AGENT_NAME", "ScriptedAgent"),
                                             "version": "1"}})
    print("initialized:", init["result"]["serverInfo"]["name"], flush=True)
    tools = rpc("tools/list")["result"]["tools"]
    print("tools:", " ".join(t["name"] for t in tools), flush=True)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        c = json.loads(line)
        r = rpc("tools/call", {"name": c["tool"], "arguments": c.get("args", {})})
        res = r.get("result", {})
        text = " ".join(x.get("text", "") for x in res.get("content", []))
        flag = " (error)" if res.get("isError") or "error" in r else ""
        print(f"result {c['tool']}{flag}: {text or r.get('error', {}).get('message', '')}",
              flush=True)
    shim.stdin.close()
    shim.wait()


if __name__ == "__main__":
    main()
