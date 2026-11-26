#!/usr/bin/env python3
"""Integration test: a Lua page's list { } and sections { } in wbconf.

A trusted page with a list (a, b, c) and suggestions (x, a): moved,
removed and added to as its buttons do (an add of a name already in it
ignored), its on_change given a table each time (said as a problem with
the count), the change listed as not saved, and Save & Apply has the page
write the names in their new order.
Its sections (one, "a=1", with t=1): one added (urgency=high) and its t
set, one added from a start ("app-name=") and renamed, moved up, the
first's t emptied: saved in that order, the emptied field left out.
Skips (exit 77) without a headless display.

Usage: wbconf_page_list_test.py <waybox> <wbconf> <schema-dir> <wbconf-lua>
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

INTEGRATION = """[Integration]
Name=Lists
Category=Notifications
Exec=true
Page=lists.lua
Files=~/.config/lists/config
"""

PAGE = r'''
page { list { "items", label = "Items", value = { "a", "b", "c" },
              choices = { "x", { "a", "The first" } } },
       sections { "rules", label = "Rules", value = { { name = "a=1", values = { t = 1 } } },
                  choices = { "urgency=high", { "app-name=", "A program" } },
                  fields = { number { "t", label = "Time" },
                             choice { "h", label = "Shown", choices = { "0", "1" } } } } }
function on_change(key, value)
  if type(value) ~= "table" then error("not a table: " .. type(value)) end
  if key == "items" then wb.problem("items", #value .. " items") end
end
function on_save(v)
  local rules = {}
  for _, r in ipairs(v.rules) do
    rules[#rules + 1] = r.name .. "(" .. (r.values.t or "") .. (r.values.h or "") .. ")"
  end
  wb.write("~/.config/lists/config", table.concat(v.items, ",") .. ";" ..
      table.concat(rules, ","))
end
'''


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, runner = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-pagelist-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    share = os.path.join(workdir, "share")
    packaged = os.path.join(share, "waybox", "integrations")
    os.makedirs(packaged)
    with open(os.path.join(packaged, "lists.integration"), "w") as f:
        f.write(INTEGRATION)
    with open(os.path.join(packaged, "lists.lua"), "w") as f:
        f.write(PAGE)
    for n in os.listdir(packaged):
        os.chmod(os.path.join(packaged, n), 0o444)
    os.chmod(packaged, 0o555)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_DIRS=share, ASAN_OPTIONS="detect_leaks=0")
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
    steps = ["luapage:lists", "luaset:lists.items=@down:0", "luaset:lists.items=@remove:2",
             "luaset:lists.items=@add:x", "luaset:lists.items=@add:a", "luaset:lists.items=@up:0",
             "luaset:lists.rules=@add:urgency=high", "luaset:lists.rules=@set:1:t=5",
             "luaset:lists.rules=@add:app-name=", "luaset:lists.rules=@rename:2:app-name=foot",
             "luaset:lists.rules=@up:2", "luaset:lists.rules=@set:0:t=",
             "luapage:lists", "changes", "save", "luapage:lists"]
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
    else:
        app.kill()
        _, err = app.communicate()
    lines = err.splitlines()
    d = [l.split("|")[1:] for l in lines if l.startswith("luapage-dump:lists")]
    rules0 = 'rules=[{"name":"a=1","values":{"t":"1"}}]'
    rules1 = ('rules=[{"name":"a=1","values":{}},{"name":"app-name=foot","values":{}},'
              '{"name":"urgency=high","values":{"t":"5"}}]')
    if len(d) != 3:
        failures.append(f"{len(d)} dumps, not 3: {lines[-10:]}")
    else:
        if d[0] != ["items=a\\nb\\nc", rules0]:
            failures.append(f"as given: {d[0]}")
        # down:0 -> b a c; remove:2 -> b a; add x -> b a x; add a: no;
        # up:0 -> no (the first): b a x.
        if d[1] != ["items=b\\na\\nx!3 items", rules1]:
            failures.append(f"after the changes: {d[1]}")
        if d[2] != d[1]:
            failures.append(f"after saving: {d[2]}")
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if not changes or "Items" not in changes[0]:
        failures.append(f"the change not listed: {changes}")
    conf = os.path.join(cfg, "lists", "config")
    if not os.path.exists(conf) or \
            open(conf).read() != "b,a,x;a=1(),app-name=foot(),urgency=high(5)":
        failures.append(f"the file as saved: {open(conf).read() if os.path.exists(conf) else None}")
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    os.chmod(packaged, 0o755)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf page list: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
