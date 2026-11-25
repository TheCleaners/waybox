#!/usr/bin/env python3
"""Integration test: wbconf saves what changed, and only that.

In a headless Waybox, wbconf opens a config with a comment, a setting of
the user's and a setting to reset. Settings are changed as the window
would (WBCONF_TEST_SET) and saved with Ctrl+S:
- the changed and the new setting are written, the reset one is gone
  (and the element it left empty);
- the comment and the untouched setting stay as they were;
- a backup of the old file is kept;
- a running Waybox is told to reload.

Skips (exit 77) without wtype, or a headless display.

Usage: wbconf_save_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <!-- my own notes -->
  <theme><name>Clearlooks</name></theme>
  <waybox>
    <switcher icons="no"/>
  </waybox>
  <mouse><libinput><naturalScroll>enabled</naturalScroll></libinput></mouse>
</openbox_config>
"""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbsave-", dir="/tmp")
    # A picker's integration file, for its key binding.
    idir = os.path.join(workdir, "share", "waybox", "integrations")
    os.makedirs(idir)
    with open(os.path.join(idir, "fcitx5.integration"), "w") as f:
        f.write("[Integration]\nName=Fcitx 5\nCategory=InputMethod\nExec=fcitx5 --replace\n"
                "Kind=inputmethod\nEnv=XMODIFIERS=@im=fcitx\n")
    with open(os.path.join(idir, "bemoji.integration"), "w") as f:
        f.write("[Integration]\nName=bemoji\nCategory=Pickers\nExec=bemoji -t -n\n"
                "Kind=binding\nKey=W-period\n")
    # wbconf only offers installed pickers: a stand-in on PATH.
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    for tool in ("bemoji", "fcitx5"):
        with open(os.path.join(bindir, tool), "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(os.path.join(bindir, tool), 0o755)
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_SET="waybox.xwayland.enabled=no;"
                                "desktops.number=6;"
                                "mouse.libinput.naturalScroll=;"
                                "bind.bemoji=W-period;"
                                "inputmethod=fcitx5",
                XDG_DATA_DIRS=os.path.join(workdir, "share"),
                PATH=bindir + ":" + os.environ.get("PATH", ""))
    failures = []
    app = None
    try:
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        time.sleep(4)
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(1.5)
        out = open(rc_path).read()
        if '<xwayland enabled="no"/>' not in out:
            failures.append("a changed setting wasn't written")
        if "<number>6</number>" not in out:
            failures.append("a new setting wasn't written")
        if "naturalScroll" in out or "<mouse>" in out:
            failures.append("a reset setting (or the element it left empty) is still there")
        if not re.search(r'<keybind key="W-period">\s*<action name="Execute">\s*'
                         r'<command>bemoji -t -n</command>', out):
            failures.append("a picker's key binding wasn't written")
        if not re.search(r'<inputMethod command="fcitx5 --replace">\s*'
                         r'<env name="XMODIFIERS" value="@im=fcitx"/>', out):
            failures.append("the input method wasn't written")
        for kept in ("<!-- my own notes -->", "<name>Clearlooks</name>",
                     '<switcher icons="no"/>'):
            if kept not in out:
                failures.append(f"{kept!r} wasn't kept as it was")
        if not os.path.exists(rc_path + ".bak") or open(rc_path + ".bak").read() != RC:
            failures.append("no backup of the previous rc.xml")
        log.flush()
        if "Reconfigured" not in open(log_path).read():
            failures.append("waybox wasn't told to reload")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if app is not None and app.poll() is None:
            app.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(open(rc_path).read(), file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf save: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
