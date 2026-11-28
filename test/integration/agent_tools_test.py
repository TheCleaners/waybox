#!/usr/bin/env python3
"""Integration test: the compositor's read tools, through the hub, to an
agent (doc/agent-tools.md: "A compositor's own tools", "Naming",
"Sensitivity and levels").

A headless Waybox with agent tools on, its manifest from the source tree,
a window open (app id wb-agent-test), and the shim as the agent ("safe"):
1. tools/list (both eras): the read tools and window_titles, named
   waybox__…, each with the manifest's description and the annotations
   its sensitivity implies; nothing else;
2. list_windows: the window, its app id, process id and that it answers,
   no title; marked with its provenance and as untrusted (app ids are an
   app's own); window_titles: ids and titles, untrusted; list_outputs and
   get_version: not marked untrusted; list_desktops;
3. a tool not offered, and the provider's own name for one: refused
   alike;
4. the screen locked: calls refused; unlocked: answered again;
5. the hub restarted: its tools are back for a new agent;
6. with no manifest installed: the hub offers none.

Skips (exit 77) without a headless display.

Usage: agent_tools_test.py <waybox> <hub> <shim> <activate_client> <lock_client> <source root>
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77
META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {}}
EXPECTED = {"waybox__list_windows", "waybox__window_titles", "waybox__list_desktops",
            "waybox__list_outputs", "waybox__get_version"}


def run_waybox(work, waybox, hub, data_dirs):
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    tools = os.path.join(work, "config", "agents", "tools")
    os.makedirs(tools)
    with open(os.path.join(tools, "settings.ini"), "w") as f:
        f.write("[Agent Tools]\nEnabled=true\n")
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=run, XDG_CONFIG_HOME=os.path.join(work, "config"),
               XDG_CONFIG_DIRS=os.path.join(work, "etc"),
               XDG_DATA_HOME=os.path.join(work, "data"), XDG_DATA_DIRS=data_dirs,
               WB_RC_XML="", WB_MENU_XML="", WAYBOX_AGENT_HUB=hub)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID", "AGENT_TOOLS_SOCKET"):
        env.pop(k, None)
    log_path = os.path.join(work, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    return proc, log, log_path, env, run


def main():
    waybox, hub, shim, activate_client, lock_client, root = sys.argv[1:7]

    def manifest_description(tool):
        """The manifest's own text for a tool (agents get exactly this)."""
        section = None
        with open(os.path.join(root, "data", "agents", "tools", "org.waybox.Waybox.ini")) as f:
            for line in f:
                line = line.strip()
                if line.startswith("["):
                    section = line[1:-1]
                elif section == f"Tool {tool}" and line.startswith("Description="):
                    return line[len("Description="):]
        return None
    failures = []
    work = tempfile.mkdtemp(prefix="waybox-agenttools-", dir="/tmp")
    proc, log, log_path, env, run = run_waybox(work, waybox, hub, os.path.join(root, "data"))
    procs = []
    work_dirs = []

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

    def agent(requests, older=False):
        """The shim, given `requests` (with _meta for the stateless era), and
        its answers by id."""
        lines = []
        if older:
            lines.append({"jsonrpc": "2.0", "id": "init", "method": "initialize",
                          "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                                     "clientInfo": {"name": "t", "version": "1"}}})
        for i, (method, params) in enumerate(requests):
            p = dict(params)
            if not older:
                p["_meta"] = META
            lines.append({"jsonrpc": "2.0", "id": i, "method": method, "params": p})
        r = subprocess.run([shim], env=env, timeout=20, capture_output=True, text=True,
                           input="".join(json.dumps(l) + "\n" for l in lines))
        out = {}
        for line in r.stdout.splitlines():
            m = json.loads(line)
            if "id" in m:
                out[m["id"]] = m
        return out

    def call(name, args=None):
        return agent([("tools/call", {"name": name, "arguments": args or {}})]).get(0, {})

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
        if not wait_for(lambda: "org.waybox.Waybox: 5 tools" in text()):
            failures.append("the hub didn't take the compositor's 5 tools")
        win = subprocess.Popen([activate_client, "wb-agent-test"], env=dict(env, WAYLAND_DISPLAY=sock),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        procs.append(win)
        if win.stdout.readline().strip() != "configured":
            failures.append("the test window didn't show")
        # 1
        for older in (False, True):
            got = agent([("tools/list", {})], older=older).get(0, {}).get("result", {})
            tools = {t["name"]: t for t in got.get("tools", [])}
            if set(tools) != EXPECTED:
                failures.append(f"tools/list (older={older}): {sorted(tools)}")
                continue
            lw = tools["waybox__list_windows"]
            if lw["description"] != manifest_description("list_windows"):
                failures.append(f"not the manifest's description: {lw['description']!r}")
            if lw["annotations"] != {"readOnlyHint": True, "destructiveHint": False,
                                     "openWorldHint": False}:
                failures.append(f"annotations: {lw['annotations']}")
        # 2
        r = call("waybox__list_windows").get("result", {})
        wins = [w for w in r.get("structuredContent", {}).get("windows", [])
                if w.get("app_id") == "wb-agent-test"]
        if len(wins) != 1 or "title" in wins[0] or not (wins[0].get("pid") or 0) > 0 or \
                wins[0].get("answers") is not True:
            failures.append(f"list_windows: {r}")
        meta = r.get("_meta", {})
        if meta.get("org.freedesktop.agent/provenance") != {
                "provider": "org.waybox.Waybox", "tool": "list_windows", "sensitivity": "read"} or \
                meta.get("org.freedesktop.agent/untrusted") is not True:
            failures.append(f"list_windows' marks: {meta}")
        r = call("waybox__window_titles").get("result", {})
        titles = r.get("structuredContent", {}).get("windows", [])
        if not titles or set(titles[0]) != {"id", "title"} or \
                r.get("_meta", {}).get("org.freedesktop.agent/untrusted") is not True:
            failures.append(f"window_titles: {r}")
        r = call("waybox__list_outputs").get("result", {})
        outs = r.get("structuredContent", {}).get("outputs", [])
        if not outs or "mode" not in outs[0] or "org.freedesktop.agent/untrusted" in r.get("_meta", {}):
            failures.append(f"list_outputs: {r}")
        r = call("waybox__get_version").get("result", {})
        if r.get("structuredContent", {}).get("name") != "waybox":
            failures.append(f"get_version: {r}")
        r = call("waybox__list_desktops").get("result", {})
        if not r.get("structuredContent", {}).get("desktops"):
            failures.append(f"list_desktops: {r}")
        # 3
        for name in ("waybox__run_command", "list_windows", "waybox__nothing"):
            e = call(name).get("error", {})
            if e.get("code") != -32602 or e.get("message") != f"unknown tool: {name}":
                failures.append(f"{name}: {e}")
        # 4
        locker = subprocess.Popen([lock_client], env=dict(env, WAYLAND_DISPLAY=sock),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        procs.append(locker)
        wait_for(lambda: "changed Locked=true" in text())
        e = call("waybox__get_version").get("error", {})
        if "locked" not in e.get("message", ""):
            failures.append(f"a call while locked: {e}")
        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        locker.wait(timeout=10)
        wait_for(lambda: "changed Locked=false" in text())
        if call("waybox__get_version").get("result", {}).get("structuredContent", {}).get("name") != "waybox":
            failures.append("calls didn't come back after unlocking")
        # 5
        pid = int(re.findall(r"started the hub \((\d+)\)", text())[-1])
        if pid > 1:
            os.kill(pid, signal.SIGKILL)
        if not wait_for(lambda: text().count("org.waybox.Waybox: 5 tools") >= 2, 10):
            failures.append("the restarted hub didn't take the compositor's tools")
        elif call("waybox__get_version").get("result", {}).get("structuredContent", {}).get("name") != "waybox":
            failures.append("the compositor's tools after a hub restart")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
        # 6
        log.close()
        work2 = tempfile.mkdtemp(prefix="waybox-agenttools-", dir="/tmp")
        proc, log, log_path, env, run = run_waybox(work2, waybox, hub, os.path.join(work2, "none"))
        if not wait_for(lambda: "none offered" in text()):
            failures.append("no manifest: the hub didn't say it offers none")
        elif agent([("tools/list", {})]).get(0, {}).get("result", {}).get("tools") != []:
            failures.append("no manifest: tools were offered anyway")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
        work_dirs.append(work2)
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        if proc.poll() is None:
            proc.kill()
        log.close()
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        print(text()[-3000:], file=sys.stderr)
        for d in [work] + work_dirs:
            shutil.rmtree(d, ignore_errors=True)
        return 1
    for d in [work] + work_dirs:
        shutil.rmtree(d, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
