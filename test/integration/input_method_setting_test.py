#!/usr/bin/env python3
"""Integration test for <waybox><inputMethod>.

1. With an input method set: Waybox starts it (as "input-method"), and it
   and the programs Waybox starts get its variables (XMODIFIERS here).
2. Reconfigure without it: it's stopped, and a variable the session had
   before (QT_IM_MODULE from the environment) comes back.
3. Never set: nothing started.

Usage: input_method_setting_test.py <waybox>
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


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    workdir = tempfile.mkdtemp(prefix="waybox-imset-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    im_env = os.path.join(workdir, "im-env")
    prog_env = os.path.join(workdir, "prog-env")
    after_env = os.path.join(workdir, "after-env")
    # The "input method": writes its variables, then stays.
    im = os.path.join(workdir, "fake-im")
    with open(im, "w") as f:
        f.write(f"#!/bin/sh\necho \"$XMODIFIERS|$QT_IM_MODULE\" > {im_env}\nexec sleep 1000\n")
    os.chmod(im, 0o755)

    def write_rc(with_im, prog_out):
        imx = (f'<inputMethod command="{im}">'
               '<env name="XMODIFIERS" value="@im=test"/>'
               '<env name="QT_IM_MODULE" value="test"/></inputMethod>') if with_im else ""
        with open(rc, "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{imx}
    <startup><program name="probe">sh -c 'echo "$XMODIFIERS|$QT_IM_MODULE" > {prog_out}'</program></startup>
  </waybox>
</openbox_config>
""")

    write_rc(True, prog_env)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=1:exitcode=99",
               QT_IM_MODULE="session-value")
    for v in ("WAYLAND_DISPLAY", "DISPLAY", "XMODIFIERS"):
        env.pop(v, None)
    lp = os.path.join(workdir, "waybox.log")
    log = open(lp, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    failures = []

    def text():
        log.flush()
        return open(lp).read()

    def read(path):
        for _ in range(30):
            if os.path.exists(path) and open(path).read().strip():
                return open(path).read().strip()
            time.sleep(0.1)
        return None

    try:
        time.sleep(1.5)
        if proc.poll() is not None:
            raise RuntimeError("waybox didn't start")
        if not re.search(r"wb-startup start input-method \d+", text()):
            failures.append("the input method wasn't started")
        if read(im_env) != "@im=test|test":
            failures.append(f"the input method didn't get its variables: {read(im_env)}")
        if read(prog_env) != "@im=test|test":
            failures.append(f"a program didn't get the input method's variables: {read(prog_env)}")

        # 2. Reconfigure without it.
        write_rc(False, after_env)
        proc.send_signal(signal.SIGUSR2)
        time.sleep(1.0)
        if "wb-startup stop input-method" not in text():
            failures.append("removing the input method didn't stop it")
        # The probe's command changed (another file): it runs again.
        got = read(after_env)
        if got != "|session-value":
            failures.append(f"the variables weren't put back after: {got!r}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    subprocess.run(["pkill", "-f", f"{workdir}/fake-im"], capture_output=True)
    contents = open(lp).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-2000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration input method setting: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
