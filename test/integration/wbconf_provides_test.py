#!/usr/bin/env python3
"""Integration test: programs that provide the same thing (Provides=).

bga and bgb both provide wallpaper; plain provides nothing.
1. "Turn Off": bga on, then bgb on: asked about bga, which goes off.
   plain on alongside: not asked.
2. "Keep Both": both on, kept; the status a plain note, not a warning,
   remembered (wbconf-view); turning bga off forgets it.
3. Both on in rc.xml, nothing asked yet: the status warns.
Skips (exit 77) without a headless display.

Usage: wbconf_provides_test.py <waybox> <wbconf> <schema-dir>
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
Name={name}
Category=Wallpaper
{provides}
Comment=A made-up program
Exec={name}
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-provides-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    share = os.path.join(workdir, "share")
    integ = os.path.join(share, "waybox", "integrations")
    os.makedirs(integ)
    for name, provides in (("bga", "Provides=wallpaper"), ("bgb", "Provides=wallpaper"),
                           ("plain", "")):
        with open(os.path.join(bindir, name), "w") as f:
            f.write("#!/bin/sh\nexec sleep 300\n")
        os.chmod(os.path.join(bindir, name), 0o755)
        with open(os.path.join(integ, name + ".integration"), "w") as f:
            f.write(INTEGRATION.format(name=name, provides=provides))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_DIRS=share, ASAN_OPTIONS="detect_leaks=0",
               PATH=bindir + ":" + os.environ.get("PATH", "/usr/bin:/bin"))
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    with open(rc, "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
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
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid))

    def run(steps, answer, extra=None):
        e = dict(wenv, WBCONF_TEST_TOC=";".join(steps), WBCONF_TEST_COMPETING=answer,
                 **(extra or {}))
        a = subprocess.Popen([wbconf], env=e, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(a)
        time.sleep(0.8 * (len(steps) + 2) + 1.5)
        a.kill()
        _, err = a.communicate()
        return [l for l in err.splitlines()
                if l.startswith(("competing-asked:", "progstatus:"))]

    failures = []
    view = os.path.join(cfg, "waybox", "wbconf-view")
    # 1. Turn Off.
    out = run(["set:program.bga=on", "set:program.bgb=on", "set:program.plain=on",
               "progstatus:bga", "progstatus:bgb"], "off")
    if out[:1] != ["competing-asked:bgb|bga"] or \
            any(l.startswith("competing-asked:plain") for l in out):
        failures.append(f"turning bgb on: {out}")
    st = [l for l in out if l.startswith("progstatus:")]
    if len(st) != 2 or not st[0].startswith("progstatus:bga|off") or \
            "warning" in st[1] or "competes" in st[1]:
        failures.append(f"after Turn Off: {st}")
    # 2. Keep Both: a note, remembered; bga off forgets it.
    out = run(["set:program.bga=on", "set:program.bgb=on", "progstatus:bgb"], "keep")
    st = [l for l in out if l.startswith("progstatus:")]
    if not st or "runs alongside bga, as you chose" not in st[0] or st[0].endswith("|warning"):
        failures.append(f"after Keep Both: {out}")
    kept = open(view).read() if os.path.exists(view) else ""
    if "kept-both=bga+bgb" not in kept:
        failures.append(f"Keep Both not remembered: {kept!r}")
    run(["set:program.bga=on", "set:program.bgb=on", "set:program.bga=off"], "keep")
    if "bga+bgb" in (open(view).read() if os.path.exists(view) else ""):
        failures.append("turning bga off didn't forget Keep Both")
    # 3. Both started by rc.xml, nothing asked: the status warns.
    with open(rc, "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc">'
                '<waybox><startup><program name="bga">bga</program>'
                '<program name="bgb">bgb</program></startup></waybox></openbox_config>\n')
    out = run(["progstatus:bga"], "keep")
    if not out or "competes with bgb: both set the wallpaper" not in out[0] or \
            not out[0].endswith("|warning") or any(l.startswith("competing-asked") for l in out):
        failures.append(f"both in rc.xml: {out}")

    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf provides: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
