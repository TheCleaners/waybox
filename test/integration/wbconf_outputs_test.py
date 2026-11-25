#!/usr/bin/env python3
"""Integration test: wbconf with hand-written <output> rules of every kind.

A headless Waybox with three outputs.

1. An rc.xml with: a rule for a monitor not connected (make+serial), one
   with a transform Waybox doesn't know and adaptiveSync="true", a second
   rule with the same matcher (never applies), one with no matcher, one
   with an odd attribute and an &, and comments. In wbconf: HEADLESS-2
   rotated 90 and flipped, adaptive sync off; HEADLESS-3 (no rule)
   adaptive sync on; HEADLESS-1's scale, with "follow" switched on, though
   it says no make, model or serial (so it can't: still by name). Saved,
   kept:
   - every rule not changed is exactly as it was, comments too;
   - HEADLESS-2's rule has the new transform and adaptiveSync="no", its
     odd attribute kept; HEADLESS-3 has a new rule; HEADLESS-1's rule has
     the scale, its unknown transform and adaptiveSync="true" as written;
   - Waybox has HEADLESS-2 flipped-90, and all three still on (a headless
     output can't do adaptive sync: asked for, it's left off, and the
     output still comes up).
2. Twin monitors (given the same make and model in wbconf's test hooks)
   share one make+model rule; a third is given a make, model and serial.
   HEADLESS-1's scale changed: it gets a rule of its own *before* the
   shared one, which is left as it was. HEADLESS-3 followed (make, model,
   serial) with a scale: a rule matching those. Saved and not answered:
   while waiting, the file has those rules; then it's back byte for byte.
3. The same, kept, then changed again and saved again in the same session:
   the rules just written are edited, none added (HEADLESS-3's one rule
   has the new scale).
Skips (exit 77) without wtype, wlr-randr, or a headless display.

Usage: wbconf_outputs_test.py <waybox> <wbconf> <schema-dir>
"""
import json
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

RC1 = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <outputs>
      <!-- the desk monitor, when docked -->
      <output make="Dell Inc." serial="5XYZ" scale="2"/>
      <output name="HEADLESS-1" transform="flipped-normal" adaptiveSync="true"/>
      <output name="HEADLESS-1" scale="3"/>
      <output enabled="no"/>
      <output name="HEADLESS-2" scale="1" note="a &amp; b"/>
    </outputs>
  </waybox>
</openbox_config>
"""

RC2 = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <outputs>
      <!-- both panels -->
      <output make="BOE" model="Twin" scale="2"/>
    </outputs>
  </waybox>
</openbox_config>
"""

UNCHANGED_1 = [
    '<!-- the desk monitor, when docked -->',
    '<output make="Dell Inc." serial="5XYZ" scale="2"/>',
    '<output name="HEADLESS-1" scale="3"/>',
    '<output enabled="no"/>',
]


def rules(text):
    """Each <output>'s attributes, in order."""
    return [dict(re.findall(r'(\w+)="([^"]*)"', m)) for m in re.findall(r'<output\b([^>]*)/>', text)]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    for tool in ("wtype", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbout-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC1)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="3", HOME=workdir,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid))
    failures = []

    def randr():
        out = subprocess.run(["wlr-randr", "--json"], env=cenv, capture_output=True,
                             text=True, timeout=10).stdout
        return {o["name"]: o for o in json.loads(out)}

    def run_wbconf(test_set, extra, then):
        app = subprocess.Popen([wbconf], env=dict(cenv, WBCONF_TEST_SET=test_set, **extra),
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        try:
            time.sleep(4)
            if app.poll() is not None:
                raise RuntimeError(f"wbconf exited ({app.returncode}): {app.stderr.read()[-500:]}")
            subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                           capture_output=True, timeout=10)
            then()
            if app.poll() is not None:
                failures.append(f"wbconf exited ({app.returncode}) after saving")
        finally:
            if app.poll() is None:
                app.kill()
            app.wait()

    try:
        # 1. Hand-written rules of every kind; three changes.
        run_wbconf("monitor.HEADLESS-2.rotation=90;monitor.HEADLESS-2.flip=on;"
                   "monitor.HEADLESS-2.adaptive=off;monitor.HEADLESS-3.adaptive=on;"
                   "monitor.HEADLESS-1.follow=on;monitor.HEADLESS-1.scale=2",
                   {"WBCONF_TEST_CHOICE": "monitors-keep"}, lambda: time.sleep(1.5))
        text = open(rc_path).read()
        for line in UNCHANGED_1:
            if line not in text:
                failures.append(f"changed or gone: {line}")
        rs = rules(text)
        two = [r for r in rs if r.get("name") == "HEADLESS-2"]
        three = [r for r in rs if r.get("name") == "HEADLESS-3"]
        want_two = {"name": "HEADLESS-2", "scale": "1", "note": "a &amp; b",
                    "transform": "flipped-90", "adaptiveSync": "no"}
        if two != [want_two]:
            failures.append(f"HEADLESS-2's rule: {two}")
        one = [r for r in rs if r.get("name") == "HEADLESS-1"]
        want_one = [{"name": "HEADLESS-1", "transform": "flipped-normal",
                     "adaptiveSync": "true", "scale": "2"}, {"name": "HEADLESS-1", "scale": "3"}]
        if one != want_one:
            failures.append(f"HEADLESS-1's rules: {one}")
        if three != [{"name": "HEADLESS-3", "adaptiveSync": "yes"}]:
            failures.append(f"HEADLESS-3's rule: {three}")
        if len(rs) != 6:
            failures.append(f"{len(rs)} rules, not 6: {rs}")
        now = randr()
        if now.get("HEADLESS-2", {}).get("transform") != "flipped-90":
            failures.append(f"Waybox's HEADLESS-2: {now.get('HEADLESS-2')}")
        log.flush()
        if "refused adaptive sync" not in open(log_path).read():
            failures.append("Waybox didn't say it left adaptive sync off")
        if not all(now.get(n, {}).get("enabled") for n in ("HEADLESS-1", "HEADLESS-2",
                                                            "HEADLESS-3")):
            failures.append(f"an output went off: {[(n, o.get('enabled')) for n, o in now.items()]}")

        # 2. Twins sharing a rule; following a monitor; not kept.
        with open(rc_path, "w") as f:
            f.write(RC2)
        proc.send_signal(signal.SIGUSR2)  # Waybox reloads (nothing to apply here)
        time.sleep(0.5)
        written = {}

        def look_then_wait():
            time.sleep(1.5)
            written["text"] = open(rc_path).read()
            time.sleep(5)
        run_wbconf("monitor-make.HEADLESS-1=BOE;monitor-model.HEADLESS-1=Twin;"
                   "monitor-serial.HEADLESS-1=A;"
                   "monitor-make.HEADLESS-2=BOE;monitor-model.HEADLESS-2=Twin;"
                   "monitor-serial.HEADLESS-2=B;"
                   "monitor-make.HEADLESS-3=LG;monitor-model.HEADLESS-3=27GL;"
                   "monitor-serial.HEADLESS-3=9;reload=1;"
                   "monitor.HEADLESS-1.scale=1;"
                   "monitor.HEADLESS-3.follow=on;monitor.HEADLESS-3.scale=1.25",
                   {"WBCONF_KEEP_SECONDS": "4"}, look_then_wait)
        rs = rules(written.get("text", ""))
        want = [{"name": "HEADLESS-1", "scale": "1"},
                {"make": "BOE", "model": "Twin", "scale": "2"},
                {"make": "LG", "model": "27GL", "serial": "9", "scale": "1.25"}]
        if rs != want:
            failures.append(f"rules written: {rs}, not {want}")
        if "<!-- both panels -->" not in written.get("text", ""):
            failures.append("the comment went")
        if open(rc_path).read() != RC2:
            failures.append("not put back exactly:\n" + open(rc_path).read())

        # 3. Saved twice in one session.
        run_wbconf("monitor-make.HEADLESS-1=BOE;monitor-model.HEADLESS-1=Twin;"
                   "monitor-serial.HEADLESS-1=A;"
                   "monitor-make.HEADLESS-2=BOE;monitor-model.HEADLESS-2=Twin;"
                   "monitor-serial.HEADLESS-2=B;"
                   "monitor-make.HEADLESS-3=LG;monitor-model.HEADLESS-3=27GL;"
                   "monitor-serial.HEADLESS-3=9;reload=1;"
                   "monitor.HEADLESS-3.follow=on;monitor.HEADLESS-3.scale=1.25;save=1;"
                   "monitor.HEADLESS-3.scale=1.5;monitor.HEADLESS-1.scale=1",
                   {"WBCONF_TEST_CHOICE": "monitors-keep"}, lambda: time.sleep(1.5))
        rs = rules(open(rc_path).read())
        want = [{"name": "HEADLESS-1", "scale": "1"},
                {"make": "BOE", "model": "Twin", "scale": "2"},
                {"make": "LG", "model": "27GL", "serial": "9", "scale": "1.5"}]
        if rs != want:
            failures.append(f"saved twice: {rs}, not {want}")
    except (RuntimeError, OSError, ValueError, KeyError) as e:
        failures.append(str(e))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(open(rc_path).read(), file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf outputs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
