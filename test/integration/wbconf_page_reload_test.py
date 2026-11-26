#!/usr/bin/env python3
"""Integration test: a Lua page's wb.reload(), as its integration's Reload=
says, done by wbconf after the save's writes.

Three integrations, installed as a package's (read-only, so trusted):
- sig: its program fakebar (a copy of sleep), Reload=signal USR2. Two
  fakebars run: one in this Wayland session, one in another. Saved: the
  one in this session gets SIGUSR2 (and, being sleep, ends of it); the
  other is left alone.
- cmd: Reload=command touch MARKER: the marker made.
- unset: no Reload=: nothing run, the status says to restart it.
Each page saves only after a change; the dump says what reload did.
Skips (exit 77) without a headless display.

Usage: wbconf_page_reload_test.py <waybox> <wbconf> <schema-dir> <wbconf-lua>
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
Name={name}
Category=Notifications
Exec={exec}
Page={id}.lua
Files=~/.config/{id}/config
{reload}
"""

PAGE = r'''
page { switch { "on", label = "On", value = (wb.read("~/.config/%s/config") or "") == "on" } }
function on_save(v)
  wb.write("~/.config/%s/config", v.on)
  wb.reload()
end
'''


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, runner = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-pagereload-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    sleep = shutil.which("sleep")
    if sleep is None:
        print("SKIP: no sleep", file=sys.stderr)
        sys.exit(SKIP)
    fakebar = os.path.join(bindir, "fakebar")
    shutil.copy(sleep, fakebar)
    marker = os.path.join(workdir, "reloaded")
    share = os.path.join(workdir, "share")
    packaged = os.path.join(share, "waybox", "integrations")
    os.makedirs(packaged)
    for id_, exec_, reload in (("sig", "fakebar", "Reload=signal USR2"),
                               ("cmd", "true", f"Reload=command touch {marker}"),
                               ("unset", "true", "")):
        with open(os.path.join(packaged, id_ + ".integration"), "w") as f:
            f.write(INTEGRATION.format(name=id_.title(), exec=exec_, id=id_, reload=reload))
        with open(os.path.join(packaged, id_ + ".lua"), "w") as f:
            f.write(PAGE % (id_, id_))
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
    # The fakebars: this session's, and another session's.
    here = subprocess.Popen([fakebar, "300"], env=dict(env, WAYLAND_DISPLAY=sock))
    wbtest.reap_on_exit(here)
    other = subprocess.Popen([fakebar, "300"], env=dict(env, WAYLAND_DISPLAY="wayland-77"))
    wbtest.reap_on_exit(other)
    steps = ["luapage:sig", "luapage:cmd", "luapage:unset",
             "luaset:sig.on=on", "luaset:cmd.on=on", "luaset:unset.on=on", "save",
             "luapage:sig", "luapage:cmd", "luapage:unset", "status"]
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

    def dumps(id_):
        return [l.split("|")[1:] for l in lines if l.startswith(f"luapage-dump:{id_}")]
    for id_, want in (("sig", "reloaded:signal:1"), ("cmd", "reloaded:command"),
                      ("unset", "reloaded:unset")):
        d = dumps(id_)
        if len(d) != 2 or d[0] != ["on="] or d[1] != ["on=on", want]:
            failures.append(f"{id_}: {d}")
        conf = os.path.join(cfg, id_, "config")
        if not os.path.exists(conf) or open(conf).read() != "on":
            failures.append(f"{id_}: the file wasn't written before reloading")
    time.sleep(0.5)
    if here.poll() != -signal.SIGUSR2:
        failures.append(f"this session's fakebar didn't get SIGUSR2: {here.poll()}")
    if other.poll() is not None:
        failures.append(f"another session's fakebar was signalled: {other.poll()}")
    if not os.path.exists(marker):
        failures.append("Reload=command didn't run")
    status = [l for l in lines if l.startswith("status-dump:")]
    if not status or "restart it to see them" not in status[-1]:
        failures.append(f"no word on restarting: {status}")
    for p in (here, other):
        if p.poll() is None:
            p.kill()
            p.wait()
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
    print("integration wbconf page reload: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
