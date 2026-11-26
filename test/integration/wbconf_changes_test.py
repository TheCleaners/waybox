#!/usr/bin/env python3
"""Integration test: wbconf's list of changes not saved yet.

wbconf with nothing changed: no list button. Then (WBCONF_TEST_SET, as if
changed in the window) a choice, a switch, a number, the GTK pointer
size, the button order made 400 characters long, the desktop names made
five lines, and more: twelve changes. Driven step by step
(WBCONF_TEST_TOC):
- the list matches the count, in page order, each "setting=before → after"
  as shown (a choice's label, on/off), every value on one short line;
- the tooltip: 8 changes, then "and 4 more", no line over ~125 characters;
- the popover: opened, it stays within the window however long the values;
- a line clicked: the popover closes and the page is at that setting.
Skips (exit 77) without a headless display.

Usage: wbconf_changes_test.py <waybox> <wbconf> <schema-dir>
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
ARROW = "→"


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbchanges-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
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
    base = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid))
    failures = []

    def run(sets, steps):
        e = dict(base, WBCONF_TEST_TOC=";".join(steps))
        if sets:
            e["WBCONF_TEST_SET"] = ";".join(sets)
        app = subprocess.Popen([wbconf], env=e, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        time.sleep(0.8 * (len(steps) + 2) + 1.5)
        if app.poll() is not None:
            _, err = app.communicate()
            failures.append(f"wbconf ended ({app.returncode}): {err[-600:]}")
            return []
        app.kill()
        _, err = app.communicate()
        return err.splitlines()

    # Nothing changed: no button, no list.
    out = run([], ["changes"])
    dumps = [l for l in out if l.startswith("changes-dump:")]
    if not dumps or not dumps[0].startswith("changes-dump:0|") or "button:hidden" not in dumps[0]:
        failures.append(f"nothing changed: {dumps}")

    long_layout = "NLIMC" + "X" * 395
    names = "One\nTwo\n\tThree\nFour\nFive"
    sets = ["placement.policy=UnderMouse", "waybox.edgeTiling.enabled=no",
            "waybox.titlebar.paddingY=7", f"theme.titleLayout={long_layout}",
            "gtk.size=40", "waybox.bell.color=#ff8800"]
    # Six more from the schema (switches and numbers that exist in it).
    extra = ["margins.top=13", "margins.bottom=14", "waybox.menu.hoverDelay=333",
             "waybox.switcher.icons=no", "waybox.menu.wrap=no", "waybox.bell.count=4"]
    steps = ["changes", "open-changes", "popover-size", "goto-change:11", "changes"]
    e_sets = sets + extra
    out = run(e_sets + [f"desktops.names={names}"], steps)
    dumps = [l for l in out if l.startswith("changes-dump:")]
    sizes = [l for l in out if l.startswith("popover-size:")]
    gotos = [l for l in out if l.startswith("goto:")]
    if len(dumps) != 2:
        failures.append(f"{len(dumps)} change dumps, not 2: {out[-20:]}")
    else:
        parts = dumps[0].split("|")
        n = int(parts[0].split(":")[1])
        items = parts[3:]
        if n < 10 or n != len(items):
            failures.append(f"count {n} vs {len(items)} lines: {parts}")
        if "button:shown" not in parts:
            failures.append(f"no button with changes: {parts[:3]}")
        want = {
            "Windows › Placement › Place new windows":
                f"Where there's most room {ARROW} Under the pointer",
            "Windows › Edge Tiling › Tile windows dragged to a screen edge":
                f"on {ARROW} off",
        }
        got = dict(i.split("=", 1) for i in items if "=" in i)
        for k, v in want.items():
            if got.get(k) != v:
                failures.append(f"{k!r}: {got.get(k)!r}, not {v!r}")
        if not any(k.endswith("Pointer size") and v == f"24 {ARROW} 40" for k, v in got.items()):
            failures.append(f"no GTK pointer size line: {items}")
        for k, v in got.items():
            if "\n" in v or len(v) > 30 * 2 + 3:
                failures.append(f"a value not kept short: {k!r} {len(v)}")
        # Page order: Appearance before Windows before Advanced.
        order = [i.split(" › ")[0] for i in items]
        firsts = [order.index(c) for c in ("Appearance", "Windows", "Advanced") if c in order]
        if firsts != sorted(firsts):
            failures.append(f"not in page order: {order}")
        tip = parts[1]
        lines, longest = (int(x) for x in tip.split(":")[1].split(","))
        if lines != 9 or longest > 125:
            failures.append(f"tooltip: {lines} lines, longest {longest}")
    if not sizes:
        failures.append("no popover size")
    else:
        w, h, ww, wh = (int(x) for x in sizes[0].split(":")[1].split(","))
        if w <= 0 or h <= 0 or w > ww or h > min(wh, 460):
            failures.append(f"popover {w}x{h} in a {ww}x{wh} window")
    if not gotos:
        failures.append("no goto")
    else:
        off, state = gotos[0].split("|")
        off = float(off.split(":")[1])
        if not -40 < off < 120 or state != "popover:closed":
            failures.append(f"after a click: {gotos[0]}")
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
    print("integration wbconf changes: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
