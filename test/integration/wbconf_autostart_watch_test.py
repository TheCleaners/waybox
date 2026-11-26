#!/usr/bin/env python3
"""Integration test: wbconf follows the autostart files changing on disk.
The user's autostart script starts mako and waybar; XDG autostart has foo
and gone. Switched in the window (mako off, gone off), then, while
wbconf runs, changed outside it: a line put above mako's (as an editor
saves: a new file renamed over), gone's file deleted, new's added.
- The lists follow: kanshi and new are there, marked new; mako's switch
  not saved yet is kept on its new line; gone's, for an entry that's gone,
  is dropped; each block has a note saying what changed.
- Saved: mako's line (now line 3) commented out, kanshi's left alone; no
  file written for gone; the notes and marks go.
- Changed again just before saving (Ctrl+S right away, before wbconf has
  read the change): waybar switched off where its line now is, foo (gone
  meanwhile) not written back.
Skips (exit 77) without wtype or a headless display.
Usage: wbconf_autostart_watch_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77
SCRIPT = "#!/bin/sh\nmako &\nwaybar &\n"
CHANGED = "#!/bin/sh\nkanshi &\nmako &\nwaybar &\n"
EXPECTED = "#!/bin/sh\nkanshi &\n#mako &\nwaybar &\n"
RACED = "#!/bin/sh\nswaybg &\nkanshi &\n#mako &\nwaybar &\n"
RACED_SAVED = "#!/bin/sh\nswaybg &\nkanshi &\n#mako &\n#waybar &\n"
ENTRY = "[Desktop Entry]\nType=Application\nName={0}\nExec={0}\n"


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbwatch-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    auto = os.path.join(cfg, "autostart")
    os.makedirs(os.path.join(cfg, "waybox"))
    os.makedirs(auto)
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    script_path = os.path.join(cfg, "waybox", "autostart")
    with open(script_path, "w") as f:
        f.write(SCRIPT)
    os.chmod(script_path, 0o755)
    for name in ("foo", "gone"):
        with open(os.path.join(auto, name + ".desktop"), "w") as f:
            f.write(ENTRY.format(name))
    sysxdg = os.path.join(workdir, "sysxdg")
    os.makedirs(os.path.join(sysxdg, "autostart"))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
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
    steps = ["set:script.2=off", "set:xdg.gone.desktop=off", "sources",
             "pause", "pause", "pause", "pause", "sources", "save", "pause", "pause", "sources",
             "set:script.4=off", "set:xdg.foo.desktop=off", "sources"]
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                XDG_CONFIG_DIRS=sysxdg, WBCONF_TEST_TOC=";".join(steps))
    failures = []
    dumps = []
    script_after_save = None
    app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    lines = queue.Queue()

    def read():
        for line in app.stderr or []:
            lines.put(line.rstrip("\n"))
    threading.Thread(target=read, daemon=True).start()
    try:
        end = time.time() + 30
        while len(dumps) < 4 and time.time() < end:
            try:
                line = lines.get(timeout=0.5)
            except queue.Empty:
                continue
            if not line.startswith("sources-dump:"):
                continue
            dumps.append(line[len("sources-dump:"):])
            if len(dumps) == 1:
                # Changed outside: as an editor saves, and a program
                # installing or removing its entry.
                tmp = script_path + ".tmp"
                with open(tmp, "w") as f:
                    f.write(CHANGED)
                os.chmod(tmp, 0o755)
                os.rename(tmp, script_path)
                os.unlink(os.path.join(auto, "gone.desktop"))
                with open(os.path.join(auto, "new.desktop"), "w") as f:
                    f.write(ENTRY.format("new"))
            if len(dumps) == 3:
                script_after_save = open(script_path).read()
            if len(dumps) == 4:
                with open(script_path, "w") as f:
                    f.write(RACED)
                os.unlink(os.path.join(auto, "foo.desktop"))
                subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                               capture_output=True, timeout=10)
                time.sleep(1.5)
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if len(dumps) != 4:
        failures.append(f"expected 4 dumps, got {dumps}")
    else:
        first, after, saved = dumps[:3]
        if script_after_save != EXPECTED:
            failures.append(f"the script isn't as expected:\n{script_after_save}")
        if first != "|script 2:mako=off* 3:waybar=on|xdg foo.desktop=on gone.desktop=off*":
            failures.append(f"before: {first}")
        want = ("|script 2:kanshi=on+ 3:mako=off* 4:waybar=on note:Changed outside wbconf "
                "just now: 1 added. Your switches not saved yet are kept."
                "|xdg foo.desktop=on new.desktop=on+ note:Changed outside wbconf just now: "
                "1 added, 1 removed. A switch you hadn't saved was for an entry that's gone, "
                "so it was dropped.")
        if after != want:
            failures.append(f"after the change:\n  got  {after}\n  want {want}")
        if saved != "|script 2:kanshi=on 3:mako=off 4:waybar=on|xdg foo.desktop=on new.desktop=on":
            failures.append(f"after saving: {saved}")
    out = open(script_path).read()
    if out != RACED_SAVED:
        failures.append(f"saved right after a change, the script isn't as expected:\n{out}")
    for name in ("gone", "foo"):
        if os.path.exists(os.path.join(auto, name + ".desktop")):
            failures.append(f"a file was written for {name}, an entry that's gone")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf autostart watch: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
