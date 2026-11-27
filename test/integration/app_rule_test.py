#!/usr/bin/env python3
"""Headless integration test for per-application rules.

Adds an <applications> rule matching foot's app_id that forces a specific
size and position, launches foot, and checks the compositor's geometry log
shows the window mapped at exactly that geometry. Exercises rule parsing +
glob matching + override application in the map handler.

With "regex": the rule's pattern is a regular expression (match="iregex",
"^FO+T$", so case is ignored and the whole id anchored), after a rule
whose regex doesn't compile (never matches, and is logged) and one that
doesn't match.

Skips (exit 77) when foot or a headless display are unavailable.

Usage: app_rule_test.py <waybox-binary> [regex]
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

RC_XML = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <application class="foot">
      <position><x>120</x><y>90</y></position>
      <size><width>500</width><height>400</height></size>
    </application>
  </applications>
</openbox_config>
"""


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    regex = len(sys.argv) == 3 and sys.argv[2] == "regex"
    rc_xml = RC_XML
    if regex:
        rc_xml = RC_XML.replace('<application class="foot">',
                                '<application match="regex" class="(unclosed">'
                                '<size><width>300</width><height>300</height></size></application>'
                                '<application match="regex" class="^bar$">'
                                '<size><width>301</width><height>301</height></size></application>'
                                '<application match="iregex" class="^FO+T$">')
    if shutil.which("foot") is None:
        print("SKIP: foot not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-ar-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(rc_xml)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99",
    )

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)

    display = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        for name in os.listdir(runtime):
            if name.startswith("wayland-") and not name.endswith(".lock"):
                display = name
                break
        if display:
            break
    if display is None:
        log.close()
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    rc = 0
    try:
        foot = subprocess.Popen(["foot"], env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(2.5)
        foot.terminate()
        time.sleep(1.0)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        print("FAIL: sanitizer/assert output", file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    m = re.search(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", contents)
    if not m:
        print("FAIL: no mapped geometry logged", file=sys.stderr)
        sys.stderr.write(contents)
        return 1
    w, h, x, y = (int(v) for v in m.groups())
    # The usable area starts at (0,0) with no panels, so position is exactly
    # the rule's x/y, and size is the rule's width/height.
    if (w, h, x, y) != (500, 400, 120, 90):
        print(f"FAIL: rule not applied: got {w}x{h}+{x}+{y}, want 500x400+120+90",
              file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    if regex and "isn't a regular expression" not in contents:
        print("FAIL: the rule whose regex doesn't compile wasn't logged", file=sys.stderr)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration app_rule: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
