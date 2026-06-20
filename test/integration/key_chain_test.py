#!/usr/bin/env python3
"""Headless integration test for chained keybindings (keytree).

Binds a two-key chain (A-a then b) to an Execute action that touches a marker
file, then drives the chain via the virtual keyboard and checks the marker
appears. Exercises the chain state machine end to end: descend on the prefix,
fire on the second key.

Skips (exit 77) when wtype or a headless display are unavailable.

Usage: key_chain_test.py <waybox-binary>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-kc-")
    marker = os.path.join(workdir, "fired")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="A-a">
      <keybind key="b">
        <action name="Execute"><command>touch {marker}</command></action>
      </keybind>
    </keybind>
  </keyboard>
</openbox_config>
""")

    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        ASAN_OPTIONS="detect_leaks=1:exitcode=99",
    )

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)

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
        time.sleep(0.5)
        # First chord: Alt+a (the chain prefix -> descend).
        subprocess.run(["wtype", "-M", "alt", "-k", "a", "-m", "alt"],
                       env=cenv, capture_output=True)
        time.sleep(0.4)
        # Second key: b (fires the chain).
        subprocess.run(["wtype", "-k", "b"], env=cenv, capture_output=True)
        for _ in range(20):
            if os.path.exists(marker):
                break
            time.sleep(0.1)
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
    if not os.path.exists(marker):
        print("FAIL: key chain A-a,b did not fire its action", file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration key_chain: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
