#!/usr/bin/env python3
"""Integration test: wbconf shows problems beside the settings they're in.

A settings tool's empty "default" cursor theme in the user's icons
(written by "nwg-look", Inherits= empty, no cursors) hides the system's;
GTK's pointer theme is TestCursors, and rc.xml has no <cursor>: Waybox's
pointer has no cursors. (A <cursor> with a size only is completed by any
save: wbconf_reload_test.) Driven step by step
(WBCONF_TEST_TOC):
1. The GTK settings' row has a note: no cursors, the file (~/...), the program
   that wrote it, and a fix ("Use TestCursors for Waybox Too"); warning
   marks on Appearance and Applications (GTK) in the sidebar, nowhere else.
2. "@problems" in the search: only that category left.
3. The fix clicked: the note says Save fixes it, no button; the list of
   changes has "Pointer for Waybox".
4. Saved: no problem left, no marks; rc.xml's <cursor> has the theme.
Skips (exit 77) without a headless display.

Usage: wbconf_problems_test.py <waybox> <wbconf> <schema-dir>
"""
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


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbproblems-", dir="/tmp")
    share = os.path.join(workdir, "share")
    datahome = os.path.join(workdir, "data")
    # In the user's icons: where both GTK and Waybox (libXcursor, which
    # doesn't read XDG_DATA_DIRS) find it.
    cursors = os.path.join(datahome, "icons", "TestCursors", "cursors")
    os.makedirs(cursors)
    with open(os.path.join(cursors, "left_ptr"), "w") as f:
        f.write("Xcur")
    default = os.path.join(datahome, "icons", "default")
    os.makedirs(default)
    default_index = os.path.join(default, "index.theme")
    with open(default_index, "w") as f:
        f.write("# This file is written by nwg-look. Do not edit.\n"
                "[Icon Theme]\nName=Default\nComment=Default Cursor Theme\nInherits=\n")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(os.path.join(cfg, "gtk-4.0"))
    with open(os.path.join(cfg, "gtk-4.0", "settings.ini"), "w") as f:
        f.write("[Settings]\ngtk-cursor-theme-name=TestCursors\ngtk-cursor-theme-size=24\n")
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc">'
                '</openbox_config>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, XDG_DATA_DIRS=share,
               XDG_DATA_HOME=datahome, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=0")
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "XCURSOR_THEME", "XCURSOR_PATH", "XCURSOR_SIZE"):
        env.pop(k, None)
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
    # Waybox exports the theme it falls back to: wbconf mustn't see it
    # here (the check looks at what Waybox was started with).
    steps = ["problems", "search:@problems", "dump", "search:", "fix:0", "problems", "changes",
             "save", "problems"]
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid), WBCONF_TEST_TOC=";".join(steps))
    failures = []
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * (len(steps) + 2) + 1.5)
    if app.poll() is not None:
        _, err = app.communicate()
        failures.append(f"wbconf ended ({app.returncode}): {err[-600:]}")
        err = ""
    else:
        app.kill()
        _, err = app.communicate()
    lines = err.splitlines()
    problems = [l for l in lines if l.startswith("problems-dump:")]
    tocs = [l.split("|")[1:] for l in lines if l.startswith("toc-dump:")]
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if len(problems) != 3:
        failures.append(f"{len(problems)} problem dumps, not 3: {lines[-12:]}")
    else:
        first, fixed, saved = problems
        if not first.startswith("problems-dump:1|"):
            failures.append(f"at the start: {first}")
        # The file, under home shown as "~".
        shown_file = "~" + default_index[len(workdir):]
        for want in ("has no cursors", shown_file, "written by nwg-look",
                     "=>Use TestCursors for Waybox Too",
                     "|toc:Appearance,Applications (GTK)"):
            if want not in first:
                failures.append(f"the note lacks {want!r}: {first}")
        if "note hidden" in first:
            failures.append(f"the note isn't shown: {first}")
        if "Save & Apply fixes it." not in fixed or not fixed.endswith(
                "=>-|toc:Appearance,Applications (GTK)"):
            failures.append(f"after the fix: {fixed}")
        if saved != "problems-dump:0|toc:":
            failures.append(f"after saving: {saved}")
    if not tocs or [x for x in tocs[0] if x[0] in "v>"] != ["vAppearance*"] and \
            [x[1:].rstrip("*") for x in tocs[0] if x[0] in "v>"] != ["Appearance"]:
        failures.append(f"@problems: {tocs[:1]}")
    if not changes or "Pointer for Waybox=default → TestCursors" not in changes[0]:
        failures.append(f"the fix isn't a change: {changes}")
    rc = open(rc_path).read()
    if not re.search(r'<cursor[^>]*theme="TestCursors"', rc):
        failures.append(f"rc.xml's <cursor> has no theme:\n{rc}")
    proc.send_signal(signal.SIGTERM)
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
    print("integration wbconf problems: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
