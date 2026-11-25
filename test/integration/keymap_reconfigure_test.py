#!/usr/bin/env python3
"""Integration test: Reconfigure applies a changed keyboard layout.

Runs Waybox on the X11 backend inside Xvfb, which gives it a real (non-
virtual) keyboard, with <keyboard><xkb><layout>us</layout></xkb>:

1. The keyboard starts with the US layout.
2. rc.xml switched to "de" and Reconfigure (SIGUSR1): the same keyboard
   now has the German layout (no restart or replug).
3. <xkb> removed and Reconfigure: it goes back to the default.

Skips (exit 77) without Xvfb.

Usage: keymap_reconfigure_test.py <waybox-binary>
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

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>{xkb}
    <keybind key="W-x"><action name="Execute"><command>true</command></action></keybind>
  </keyboard>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("Xvfb") is None:
        print("SKIP: Xvfb not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-keymap-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")

    def write_rc(layout):
        xkb = f"\n    <xkb><layout>{layout}</layout></xkb>" if layout else ""
        with open(rc, "w") as f:
            f.write(RC.format(xkb=xkb))

    write_rc("us")
    rfd, wfd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(wfd), "-screen", "0",
                             "1024x768x24", "-nolisten", "tcp", "-noreset"],
                            pass_fds=[wfd], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    os.close(wfd)
    num = os.read(rfd, 16).decode().strip()
    os.close(rfd)
    if not num:
        xvfb.kill()
        print("SKIP: Xvfb did not start", file=sys.stderr)
        sys.exit(SKIP)

    env = dict(os.environ, DISPLAY=f":{num}", WLR_BACKENDS="x11", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    for var in ("WAYLAND_DISPLAY", "XKB_DEFAULT_LAYOUT", "XKB_DEFAULT_VARIANT",
                "XKB_DEFAULT_OPTIONS", "XKB_DEFAULT_MODEL", "XKB_DEFAULT_RULES"):
        env.pop(var, None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    failures = []

    def keymaps():
        log.flush()
        return re.findall(r"wb-keymap (.+)", open(log_path).read())

    def wait_keymaps(n, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if len(keymaps()) >= n:
                break
            time.sleep(0.1)
        return keymaps()

    try:
        seen = wait_keymaps(1)
        if not seen:
            sys.stderr.write(open(log_path).read()[-2000:])
            print("SKIP: no keyboard on the X11 backend", file=sys.stderr)
            proc.kill()
            xvfb.kill()
            sys.exit(SKIP)
        if "English (US)" not in seen[-1]:
            failures.append(f"starting layout is {seen[-1]!r}, not US")

        write_rc("de")
        proc.send_signal(signal.SIGUSR1)
        seen = wait_keymaps(len(seen) + 1)
        if "German" not in seen[-1]:
            failures.append(f"after Reconfigure to de the layout is {seen[-1]!r}")

        write_rc(None)
        proc.send_signal(signal.SIGUSR1)
        seen = wait_keymaps(len(seen) + 1)
        if "German" in seen[-1]:
            failures.append("removing <xkb> didn't go back to the default layout")
        if proc.poll() is not None:
            failures.append("waybox exited")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        xvfb.kill()
        xvfb.wait()

    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration keymap reconfigure: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
