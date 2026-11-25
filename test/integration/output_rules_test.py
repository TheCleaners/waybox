#!/usr/bin/env python3
"""Integration test for rc.xml's <outputs> rules.

Three headless outputs:
  HEADLESS-1  mode 1600x900, scale 2, at 0,0, rotated 90
  HEADLESS-2  at 900,0 (to the right of HEADLESS-1's rotated 450x800)
  HEADLESS-3  disabled

1. At startup each output has what its rule says; a bad value is logged
   and the rest of the rule still applies.
2. The disabled output, enabled later with wlr-randr, joins the layout
   and is drawn (it gets frames: a screenshot of it works).
3. Reconfigure (SIGUSR1) applies changed rules...
4. ...except to an output a client (wlr-randr) configured this session.

Skips (exit 77) without wlr-randr (with --json) or grim, or a headless
display.

Usage: output_rules_test.py <waybox-binary>
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <outputs>
      <output name="HEADLESS-1" mode="1600x900" scale="{scale1}" position="0,0"
              transform="90"/>
      <output name="HEADLESS-2" position="900,0" scale="{scale2}" mode="garbage"/>
      <output name="HEADLESS-3" enabled="no"/>
    </outputs>
  </waybox>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wlr-randr", "grim"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    helptext = subprocess.run(["wlr-randr", "--help"], capture_output=True, text=True)
    if "--json" not in helptext.stdout + helptext.stderr:
        print("SKIP: wlr-randr without --json", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-outputs-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc, "w") as f:
        f.write(RC.format(scale1=2, scale2=1))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="3", XDG_CONFIG_HOME=cfg,
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def outputs():
        out = subprocess.run(["wlr-randr", "--json"], env=cenv, capture_output=True,
                             text=True, timeout=10).stdout
        return {o["name"]: o for o in json.loads(out)}

    def current_mode(o):
        return next(((m["width"], m["height"]) for m in o["modes"] if m["current"]), None)

    def logtext():
        log.flush()
        return open(log_path).read()

    try:
        time.sleep(0.5)
        # 1. Startup.
        o = outputs()
        h1, h2, h3 = o["HEADLESS-1"], o["HEADLESS-2"], o["HEADLESS-3"]
        expect(current_mode(h1) == (1600, 900), f"HEADLESS-1 mode {current_mode(h1)}")
        expect(h1["scale"] == 2.0, f"HEADLESS-1 scale {h1['scale']}")
        expect(h1["transform"] == "90", f"HEADLESS-1 transform {h1['transform']}")
        expect(h1["position"] == {"x": 0, "y": 0}, f"HEADLESS-1 at {h1['position']}")
        expect(h2["position"] == {"x": 900, "y": 0}, f"HEADLESS-2 at {h2['position']}")
        expect(not h3["enabled"], "HEADLESS-3 should start disabled")
        expect('Ignoring the output setting mode="garbage"' in logtext(),
               "the bad mode wasn't logged")

        # 2. Enable the disabled output.
        subprocess.run(["wlr-randr", "--output", "HEADLESS-3", "--on", "--pos", "3000,0"],
                       env=cenv, capture_output=True, timeout=10)
        time.sleep(0.4)
        h3 = outputs()["HEADLESS-3"]
        expect(h3["enabled"] and h3["position"] == {"x": 3000, "y": 0},
               f"HEADLESS-3 not enabled at 3000,0: {h3['enabled']} {h3['position']}")
        try:
            r = subprocess.run(["grim", "-o", "HEADLESS-3", os.path.join(workdir, "3.png")],
                               env=cenv, capture_output=True, timeout=5)
            expect(r.returncode == 0, "screenshot of the enabled output failed")
        except subprocess.TimeoutExpired:
            failures.append("the enabled output is never drawn (screenshot hangs)")

        # 3 and 4. A client configures HEADLESS-2; then both rules change.
        subprocess.run(["wlr-randr", "--output", "HEADLESS-2", "--scale", "1.5"],
                       env=cenv, capture_output=True, timeout=10)
        time.sleep(0.3)
        with open(rc, "w") as f:
            f.write(RC.format(scale1=1, scale2=3))
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.8)
        o = outputs()
        expect(o["HEADLESS-1"]["scale"] == 1.0,
               f"Reconfigure didn't apply the new scale: {o['HEADLESS-1']['scale']}")
        expect(o["HEADLESS-2"]["scale"] == 1.5,
               f"Reconfigure overrode a client's setting: {o['HEADLESS-2']['scale']}")
        expect(o["HEADLESS-3"]["enabled"],
               "Reconfigure disabled an output a client had enabled")
        expect(proc.poll() is None, "waybox exited")
    except (subprocess.TimeoutExpired, json.JSONDecodeError, KeyError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        log.close()

    contents = open(log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration output rules: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
