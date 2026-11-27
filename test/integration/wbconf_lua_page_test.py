#!/usr/bin/env python3
"""Integration test: an integration's settings page written in Lua, in wbconf.

Integration files with Page=NAME.lua, two as a package installs them
(trusted), one in the user's own folder:
- demo: its page reads ~/.config/demo/config (Files=); may run touch
  (Runs=). wbconf (driven by WBCONF_TEST_TOC) shows its values from the
  file; a timeout under a second shows a problem (on_change), and the
  change is listed as not saved; its button runs touch; Save & Apply has
  wbconf write the new file (the old one kept as config.bak) and run the
  page's reload (touch).
- broken: its script has a syntax error: the page says so, with the line,
  and wbconf goes on.
- mine (the user's folder): its page waits, its runner not started,
  until allowed; then it runs, and is remembered; changed, it's asked
  about again.
Then a runner gone wrong (a fake one): wbconf refuses its write outside
Files= and its run outside Runs=; and one that never answers: wbconf says
so after 3 s and goes on.
Skips (exit 77) without a headless display.

Usage: wbconf_lua_page_test.py <waybox> <wbconf> <schema-dir> <wbconf-lua>
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

DEMO = """[Integration]
Name=Demo
Category=Notifications
Comment=A page written in Lua
Exec=true
Page=demo.lua
Files=~/.config/demo/config
Runs=touch
"""

DEMO_LUA = r'''
local text = wb.read("~/.config/demo/config") or ""
local home = text:match("marker=([^\n]+)") or ""

page {
  group "Timing" {
    number { "timeout", label = "Disappear after", unit = "ms", min = 0, max = 60000,
             value = text:match("timeout=(%d+)") or "5000" },
  },
  switch { "icons", label = "Icons", value = text:match("icons=yes") ~= nil },
  button { "Try it", key = "test", on_click = function()
    wb.run { "touch", home .. "/clicked" }
  end },
}

function on_change(key, value)
  if key == "timeout" then
    wb.problem("timeout", tonumber(value) < 1000 and "Under a second" or "")
  end
end

function on_save(v)
  wb.write("~/.config/demo/config", "marker=" .. home .. "\ntimeout=" .. v.timeout ..
      "\nicons=" .. (v.icons == "on" and "yes" or "no") .. "\n")
  wb.run { "touch", home .. "/reloaded" }
end
'''

BROKEN = """[Integration]
Name=Broken
Category=Notifications
Exec=true
Page=broken.lua
"""

BROKEN_LUA = "page {\n  number { \"x\", value = 1 \n"

# In the user's own folder: run only once allowed.
MINE = """[Integration]
Name=Mine
Category=Notifications
Exec=true
Page=mine.lua
Runs=true
"""

MINE_LUA = 'page { switch { "on", label = "On", value = true } }\n'

# Full access: it may run a shell.
RISKY = MINE.replace("Name=Mine", "Name=Risky").replace("mine.lua", "risky.lua") \
    .replace("Runs=true", "Runs=true, sh")
# In a data folder the user can write.
LOCAL = MINE.replace("Name=Mine", "Name=Local").replace("mine.lua", "local.lua")

# A runner gone wrong (wbconf checks again what it asks): its button asks
# to run a program not in Runs=, its save to write a file not in Files=. With
# "stuck", it never answers at all.
ROGUE = r'''#!/usr/bin/env python3
import json, sys, time
stuck = len(sys.argv) > 1
for line in sys.stdin:
    m = json.loads(line)
    if stuck:
        time.sleep(60)
    out = []
    if "open" in m:
        out.append({"page": {"controls": [{"kind": "number", "key": "n", "label": "N",
                                            "value": "1"},
                                           {"kind": "button", "key": "b", "label": "B"}]}})
    if "click" in m:
        out.append({"run": ["mkdir", "OUTSIDE.click"]})  # mkdir: not in Runs=
    if "save" in m:
        out.append({"write": {"path": "OUTSIDE", "text": "owned"}})
        out.append({"run": ["touch", "OUTSIDE.run"]})
    for o in out + [{"done": True}]:
        print(json.dumps(o), flush=True)
'''



def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, runner = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbluapage-", dir="/tmp")
    home = workdir
    cfg = os.path.join(workdir, "config")
    # demo and broken as a package installs them (a data folder: trusted);
    # mine in the user's own folder (asked about first).
    share = os.path.join(workdir, "share")
    packaged = os.path.join(share, "waybox", "integrations")
    integ = os.path.join(cfg, "waybox", "integrations")
    os.makedirs(packaged)
    os.makedirs(integ)
    for name, text in (("demo.integration", DEMO), ("demo.lua", DEMO_LUA),
                       ("broken.integration", BROKEN), ("broken.lua", BROKEN_LUA)):
        with open(os.path.join(packaged, name), "w") as f:
            f.write(text)
    for name, text in (("mine.integration", MINE), ("mine.lua", MINE_LUA),
                       ("risky.integration", RISKY), ("risky.lua", MINE_LUA)):
        with open(os.path.join(integ, name), "w") as f:
            f.write(text)
    # A package's files can't be changed by the user: trusted. A data folder
    # the user can write (Waybox installed into ~/.local): asked about.
    for root, dirs, files in os.walk(packaged):
        for n in files:
            os.chmod(os.path.join(root, n), 0o444)
    os.chmod(packaged, 0o555)
    local = os.path.join(workdir, "localshare", "waybox", "integrations")
    os.makedirs(local)
    for name, text in (("local.integration", LOCAL), ("local.lua", MINE_LUA)):
        with open(os.path.join(local, name), "w") as f:
            f.write(text)
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    conf = os.path.join(home, ".config", "demo", "config")  # ~/.config, as Files= says
    os.makedirs(os.path.dirname(conf))
    original = f"marker={workdir}\ntimeout=7000\nicons=yes\n"
    with open(conf, "w") as f:
        f.write(original)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_DIRS=share + ":" + os.path.join(workdir, "localshare"),
               ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
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
    steps = ["luapage:demo", "luapage:broken", "luaset:demo.timeout=500", "luapage:demo",
             "changes", "luaset:demo.click=test", "save", "luapage:demo", "changes"]
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid), WBCONF_LUA=runner,
                WBCONF_TEST_TOC=";".join(steps))
    failures = []
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * (len(steps) + 2) + 1.5)
    if app.poll() is not None:
        _, err = app.communicate()
        failures.append(f"wbconf ended ({app.returncode}): {err[-800:]}")
        err = ""
    else:
        app.kill()
        _, err = app.communicate()
    lines = err.splitlines()
    dumps = [l.split("|")[1:] for l in lines if l.startswith("luapage-dump:demo")]
    broken = [l for l in lines if l.startswith("luapage-dump:broken")]
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if len(dumps) != 3:
        failures.append(f"{len(dumps)} demo dumps, not 3: {lines[-15:]}")
    else:
        if dumps[0] != ["timeout=7000", "icons=on"]:
            failures.append(f"the page from the file: {dumps[0]}")
        if "timeout=500!Under a second" not in dumps[1]:
            failures.append(f"a problem from on_change: {dumps[1]}")
        if "timeout=500!Under a second" not in dumps[2] or any(d.startswith("error") for d in dumps[2]):
            failures.append(f"after saving: {dumps[2]}")
    if not broken or "broken.lua:" not in broken[0] or "error:" not in broken[0]:
        failures.append(f"the broken page: {broken}")
    if len(changes) != 2 or "Disappear after=7000 → 500" not in changes[0]:
        failures.append(f"the change not listed: {changes[:1]}")
    elif "Disappear after" in changes[1]:
        failures.append(f"still listed after saving: {changes[1]}")
    if not os.path.exists(os.path.join(workdir, "clicked")):
        failures.append("the button's program didn't run")
    if not os.path.exists(os.path.join(workdir, "reloaded")):
        failures.append("the save's program didn't run")
    text = open(conf).read()
    if text != f"marker={workdir}\ntimeout=500\nicons=yes\n":
        failures.append(f"the file as saved: {text!r}")
    bak = conf + ".bak"
    if not os.path.exists(bak) or open(bak).read() != original:
        failures.append("no backup of the file as it was")
    # Approval: mine waits (its runner not started), runs once allowed, is
    # remembered; changed, it's asked about again.
    approvals = os.path.join(cfg, "waybox", "approved-pages")

    def mine_dumps(steps3):
        e3 = dict(wenv, WBCONF_TEST_TOC=";".join(steps3))
        a = subprocess.Popen([wbconf], env=e3, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(a)
        time.sleep(0.8 * (len(steps3) + 2) + 1.5)
        a.kill()
        _, e = a.communicate()
        return [l.split("|")[1:] for l in e.splitlines() if l.startswith("luapage-dump:mine")]

    # Saved while still asking: said for a program in use (Local, started),
    # not for one that isn't (Risky).
    e_all = dict(wenv, WBCONF_TEST_TOC="luapage:local;luapage:risky;set:program.local=on;"
                 "save;status")
    a = subprocess.Popen([wbconf], env=e_all, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(a)
    time.sleep(0.8 * 7 + 1.5)
    a.kill()
    _, e = a.communicate()
    local_d = [l for l in e.splitlines() if l.startswith("luapage-dump:local")]
    risky_d = [l for l in e.splitlines() if l.startswith("luapage-dump:risky")]
    if local_d != ["luapage-dump:local|approval:waiting"]:
        failures.append(f"a page in a folder the user can write ran unasked: {local_d}")
    if risky_d != ["luapage-dump:risky|approval:waiting:full"]:
        failures.append(f"full access not said: {risky_d}")
    status = [l for l in e.splitlines() if l.startswith("status-dump:")]
    if not status or "Local's settings page isn't allowed yet" not in status[-1] or \
            "Risky" in status[-1]:
        failures.append(f"saving while asking not said (or said for an unused one): {status}")

    d = mine_dumps(["luapage:mine", "luaset:mine.approve=1", "luapage:mine"])
    if len(d) != 2 or d[0] != ["approval:waiting"] or d[1] != ["on=on"]:
        failures.append(f"approval: {d}")
    if "mine " not in (open(approvals).read() if os.path.exists(approvals) else ""):
        failures.append("the approval wasn't remembered")
    d = mine_dumps(["luapage:mine"])
    if d != [["on=on"]]:
        failures.append(f"allowed before, asked again: {d}")
    with open(os.path.join(integ, "mine.lua"), "a") as f:
        f.write("-- changed\n")
    d = mine_dumps(["luapage:mine"])
    if d != [["approval:changed"]]:
        failures.append(f"changed, not asked again: {d}")

    # A runner gone wrong: wbconf refuses what isn't allowed. One that never
    # answers: wbconf gives up on it after 3 s and goes on.
    outside = os.path.join(workdir, "outside")
    for name, args in (("rogue", ""), ("stuck", " stuck")):
        fake = os.path.join(workdir, name + "-runner")
        with open(fake, "w") as f:
            # Its stderr not wbconf's: a stuck one would hold the test's pipe.
            f.write("#!/bin/sh\nexec " + sys.executable + " " + fake + ".py" + args +
                    " 2>/dev/null\n")
        os.chmod(fake, 0o755)
        with open(fake + ".py", "w") as f:
            f.write(ROGUE.replace("OUTSIDE", outside))
        steps2 = ["luaset:demo.click=b", "luaset:demo.n=5", "save", "luapage:demo"]
        e2 = dict(wenv, WBCONF_LUA=fake, WBCONF_TEST_TOC=";".join(steps2))
        t0 = time.time()
        app = subprocess.Popen([wbconf], env=e2, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        time.sleep(0.8 * (len(steps2) + 2) + (8 if name == "stuck" else 1.5))
        alive = app.poll() is None
        # Given up on, the stuck runner is stopped, not left sleeping (asked
        # while wbconf still runs: killing wbconf would orphan it anyway).
        if name == "stuck":
            left = subprocess.run(["pgrep", "-f", fake + ".py"], capture_output=True,
                                  text=True).stdout.split()
            if left:
                failures.append(f"stuck: its runner still runs ({left})")
                for pid in left:
                    try:
                        os.kill(int(pid), signal.SIGKILL)
                    except (ProcessLookupError, ValueError):
                        pass
        app.kill()
        _, err2 = app.communicate()
        if not alive:
            failures.append(f"{name}: wbconf ended")
        if any(os.path.exists(outside + x) for x in ("", ".run", ".click")):
            failures.append(f"{name}: wbconf did what the page wasn't allowed to")
        dump = [l for l in err2.splitlines() if l.startswith("luapage-dump:demo")]
        if name == "stuck" and (not dump or "didn't answer in time" not in dump[0]):
            failures.append(f"stuck: not said: {dump} after {time.time() - t0:.0f}s")
        if name == "rogue" and "isn't allowed" not in err2 and "isn't in the integration" \
                not in err2:
            failures.append(f"rogue: not refused: {err2[-400:]}")

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    # Writable again, to be removed.
    os.chmod(packaged, 0o755)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf lua page: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
