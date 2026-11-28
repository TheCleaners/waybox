#!/usr/bin/env python3
"""Integration test: calls through the hub (doc/agent-tools.md, "Calls",
"The audit log"), with a stand-in compositor (agent_harness.py) whose
tools are slow on purpose; small tunables keep it quick.

1. CallTimeout: a call nothing is heard of ends as outcome unknown, the
   agent told it may or may not have taken effect, the provider asked to
   stop;
2. progress: passed on with the agent's token, and each one keeps the
   call alive past CallTimeout;
3. CallDeadline: progress doesn't extend a call past it;
4. MaxInFlight: at most that many run at once, calls given up on (still
   running at a provider that ignores cancellation) included; the rest
   wait and run;
5. cancelled after dispatch: no answer to the agent, the provider asked
   to stop, the call recorded as cancellation requested;
6. the provider going away mid-call: outcome unknown;
7. CallRate: calls past it are refused;
8. the screen locked: refused; unlocked: answered;
9. the audit: only the user's; the hub's start, connections, each call's
   end with its outcome; a Secret redacted, a long string cut.

Usage: agent_calls_test.py <hub> <compositor interface XML>
"""
import os
import shutil
import stat
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from agent_harness import Harness
except (ImportError, ValueError):
    print("SKIP: no Python GObject bindings", file=sys.stderr)
    sys.exit(77)

UNKNOWN = "may or may not have taken effect"


def main():
    hub, xml = sys.argv[1:3]
    work = tempfile.mkdtemp(prefix="waybox-agentcalls-", dir="/tmp")
    cfg = os.path.join(work, "config", "agents", "tools")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "settings.ini"), "w") as f:
        f.write("[Tunables]\nCallTimeout=400 ms\nCallDeadline=1500 ms\nMaxInFlight=2\nCallRate=30\n")
    sysdir = os.path.join(work, "etc", "agents", "tools")
    os.makedirs(sysdir)
    with open(os.path.join(sysdir, "settings.ini"), "w") as f:
        f.write("[Tunables]\nAuditCut=256\n")
    h = Harness(hub, work, xml)
    failures = []

    def ends(rid, agent, timeout=10):
        return agent.answer(rid, timeout)

    def is_unknown(a):
        return a is not None and a.get("result", {}).get("isError") is True and \
            UNKNOWN in a["result"]["content"][0]["text"]

    def audit_for(call_text):
        return [l for l in h.audit() if l.get("event") == "call" and l.get("tool") == call_text]

    try:
        a = h.agent()
        if not a.handshake.get("ok"):
            failures.append(f"handshake: {a.handshake}")
            return 1
        for _ in range(100):
            if "org.test.Harness: 3 tools" in h.hub_log():
                break
            time.sleep(0.05)
        # 1
        t0 = time.time()
        a.call(1, "harness__sleep", {"ms": 1500})
        r = ends(1, a)
        took = time.time() - t0
        if not is_unknown(r) or not 0.3 < took < 1.4:
            failures.append(f"CallTimeout: {r} after {took:.1f} s")
        time.sleep(0.2)
        if not h.cancelled:
            failures.append("the provider wasn't asked to stop at CallTimeout")
        # 2
        a.call(2, "harness__sleep", {"ms": 1000, "progress_ms": 150}, progress_token="tok-2")
        r = ends(2, a)
        progress = [m for m in a.messages if m.get("method") == "notifications/progress"
                    and m["params"].get("progressToken") == "tok-2"]
        if r is None or r.get("result", {}).get("isError") or len(progress) < 3:
            failures.append(f"progress: {r}, {len(progress)} progress notifications")
        # 3
        t0 = time.time()
        a.call(3, "harness__sleep", {"ms": 2500, "progress_ms": 150}, progress_token="tok-3")
        r = ends(3, a)
        took = time.time() - t0
        if not is_unknown(r) or not 1.2 < took < 2.4:
            failures.append(f"CallDeadline: {r} after {took:.1f} s")
        # 4: the provider ignores requests to stop, so the calls the hub
        # gave up on (1, 3) still run there, and still count: never more
        # than MaxInFlight at once, from the start (max_running isn't
        # reset), and a new call waits behind them.
        t0 = time.time()
        for i in range(5):
            a.call(10 + i, "harness__sleep", {"ms": 200})
        got = [ends(10 + i, a) for i in range(5)]
        if any(g is None or g.get("result", {}).get("isError") for g in got) or h.max_running != 2:
            failures.append(f"MaxInFlight: {h.max_running} at once; {got}")
        # (call 3 runs to 2.5 s at the provider: these waited for its slot)
        if time.time() - t0 < 0.5:
            failures.append(f"calls didn't wait for the slots given-up calls held: {time.time() - t0:.1f} s")
        # 5
        for _ in range(100):
            if h.running == 0:
                break
            time.sleep(0.05)
        before = len(h.cancelled)
        a.call(20, "harness__sleep", {"ms": 400})
        time.sleep(0.1)
        a.cancel(20)
        time.sleep(0.6)
        if a.answer(20, 0.1) is not None:
            failures.append("an answer after the agent cancelled")
        if len(h.cancelled) <= before:
            failures.append("the provider wasn't asked to stop after a cancellation")
        # 7 (before 6: 6 takes the provider away)
        rate = [m for m in a.messages if "id" in m]
        used = len([m for m in rate if m.get("id") not in (20,)]) + 1  # (20 was cancelled)
        for i in range(30 - used + 2):
            a.call(100 + i, "harness__echo", {"text": "x"})
        last = [ends(100 + i, a) for i in range(30 - used + 2)]
        refused = [x for x in last if x and "CallRate" in x.get("error", {}).get("message", "")]
        if not refused:
            failures.append(f"CallRate: none refused of {len(last)} past {used}")
        # 8
        b = h.agent()
        h.set_locked(True)
        time.sleep(0.15)
        b.call(1, "harness__echo", {"text": "locked?"})
        r = ends(1, b)
        if r is None or "locked" not in r.get("error", {}).get("message", ""):
            failures.append(f"a call while locked: {r}")
        h.set_locked(False)
        time.sleep(0.15)
        b.call(2, "harness__echo", {"password": "hunter2", "text": "y" * 1000})
        if ends(2, b) is None:
            failures.append("calls didn't come back after unlocking")
        # 6
        b.call(3, "harness__sleep", {"ms": 1200})
        time.sleep(0.2)
        h.drop_tools()
        r = ends(3, b)
        if not is_unknown(r) or "provider went away" not in r["result"]["content"][0]["text"]:
            failures.append(f"the provider going away: {r}")
        a.close()
        b.close()
        time.sleep(0.5)
        # 9
        adir = os.path.join(h.state_dir, "agents", "tools")
        apath = os.path.join(adir, "audit.log")
        if stat.S_IMODE(os.stat(apath).st_mode) != 0o600 or stat.S_IMODE(os.stat(adir).st_mode) != 0o700:
            failures.append("the audit isn't only the user's")
        lines = h.audit()
        events = [l.get("event") for l in lines]
        for e in ("hub", "connect", "disconnect", "call"):
            if e not in events:
                failures.append(f"no {e} line in the audit")
        outcomes = {l.get("outcome") for l in audit_for("sleep")}
        for o in ("outcome-unknown", "completed", "cancellation-requested"):
            if o not in outcomes:
                failures.append(f"no sleep call ended {o} in the audit: {outcomes}")
        if not any(l.get("why") == "rate" for l in audit_for("echo")) or \
                not any(l.get("why") == "locked" for l in audit_for("echo")):
            failures.append("the refusals (rate, locked) aren't in the audit")
        sec = [l for l in audit_for("echo") if isinstance(l.get("args"), dict) and "password" in l["args"]]
        if not sec or sec[0]["args"]["password"] != "<redacted>" or \
                sec[0]["args"]["text"].get("cut") is not True or sec[0]["args"]["text"].get("length") != 1000:
            failures.append(f"the audit's arguments: {sec[:1]}")
        if any("hunter2" in str(l) for l in lines):
            failures.append("a secret reached the audit")
    finally:
        h.stop()
    if failures and os.environ.get("WB_KEEP_WORK"):
        print(f"kept: {work}", file=sys.stderr)
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        return 1
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        print(h.hub_log()[-3000:], file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
