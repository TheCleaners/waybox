#!/usr/bin/env python3
"""Integration test: a Lua page reads its program's command (wb.option).

An integration "opt" (installed as a package's, so trusted) with
[Option config] (-c, a file) and a page editing the file its command names:
1. rc.xml starts it as "optprog -c FIRST": the page shows FIRST and reads it,
   though FIRST isn't in Files=; saving writes FIRST.
2. The option set to SECOND (no unsaved changes): the page opens again, for
   SECOND.
3. A change not saved, then the option set back to FIRST: the page stays as
   it was, and says why.
Skips (exit 77) without a headless display.

Usage: wbconf_page_option_test.py <waybox> <wbconf> <schema-dir> <wbconf-lua>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

INTEGRATION = """[Integration]
Name=Opt
Category=Notifications
Exec=optprog
Page=opt.lua
Files=~/.config/opt/config

[Option config]
Label=Config file
Flag=-c
Type=file
"""

PAGE = r'''
local path = wb.option("config") or "~/.config/opt/config"
page { text { "path", label = "Path", value = path },
       switch { "on", label = "On", value = (wb.read(path) or "") == "on" } }
function on_save(v)
  wb.write(path, v.on == "on" and "on" or "off")
end
'''


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, runner = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-pageoption-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(os.path.join(cfg, "waybox"))
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    optprog = os.path.join(bindir, "optprog")
    with open(optprog, "w") as f:
        f.write("#!/bin/sh\nexec sleep 300\n")
    os.chmod(optprog, 0o755)
    first = os.path.join(workdir, "first.conf")
    second = os.path.join(workdir, "second.conf")
    with open(first, "w") as f:
        f.write("on")
    with open(second, "w") as f:
        f.write("on")
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc">\n'
                f'  <waybox><startup><program name="opt">optprog -c {first}</program>'
                '</startup></waybox>\n</openbox_config>\n')
    share = os.path.join(workdir, "share")
    packaged = os.path.join(share, "waybox", "integrations")
    os.makedirs(packaged)
    with open(os.path.join(packaged, "opt.integration"), "w") as f:
        f.write(INTEGRATION)
    with open(os.path.join(packaged, "opt.lua"), "w") as f:
        f.write(PAGE)
    for n in os.listdir(packaged):
        os.chmod(os.path.join(packaged, n), 0o444)
    os.chmod(packaged, 0o555)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_DIRS=share, ASAN_OPTIONS="detect_leaks=0",
               PATH=bindir + ":" + os.environ.get("PATH", "/usr/bin:/bin"))
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
    steps = ["luapage:opt",                                   # 1. FIRST, on
             "luaset:opt.on=", "save", "luapage:opt",         #    saved: FIRST off
             f"set:option.opt.config={second}", "wait", "luapage:opt",   # 2. SECOND, on
             "luaset:opt.on=", f"set:option.opt.config={first}", "wait",
             "luapage:opt"]                                    # 3. unchanged, said why
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
    dumps = [l.split("|")[1:] for l in err.splitlines() if l.startswith("luapage-dump:opt|")]
    want = [[f"path={first}", "on=on"],
            [f"path={first}", "on="],
            [f"path={second}", "on=on"]]
    if len(dumps) != 4 or dumps[:3] != want:
        failures.append(f"the page's values: {dumps}")
    elif dumps[3][:2] != [f"path={second}", "on="] or \
            not any("Its command changed" in d for d in dumps[3]):
        failures.append(f"with a change not saved, the page reopened or didn't say: "
                        f"{dumps[3]}")
    if open(first).read() != "off":
        failures.append(f"FIRST (named by -c, not in Files=) not written: "
                        f"{open(first).read()!r}")
    if open(second).read() != "on":
        failures.append("SECOND written")
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
    print("integration wbconf page option: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
