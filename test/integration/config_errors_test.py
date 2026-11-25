#!/usr/bin/env python3
"""Integration test: a broken rc.xml is refused, never half-read.

1. Started with a broken rc.xml (one missing quote), Waybox runs on its
   defaults and logs the file, line and error.
2. With a good rc.xml, a key binding (Super+T runs a command) works; after
   rc.xml is broken and Waybox reconfigured (SIGUSR1), the log says the
   current settings are kept, and the binding still works. (Read leniently,
   the broken file lost the whole <keyboard> section.)
3. An empty rc.xml is no error: it just has no settings.

D-Bus is pointed at nothing, so the error notifications Waybox sends can't
reach a real desktop.

Skips (exit 77) without wtype, or a headless display.

Usage: config_errors_test.py <waybox-binary>
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

GOOD = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <keyboard>
    <keybind key="W-t"><action name="Execute"><command>touch {flag}</command></action></keybind>
  </keyboard>
</openbox_config>
"""
# One missing closing quote before <keyboard>.
BROKEN = GOOD.replace("<desktops><number>2</number></desktops>",
                      '<desktops><number>2</number></desktops>\n  <margins top="5>\n  </margins>')


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-rcerr-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc = os.path.join(cfg, "waybox", "rc.xml")
    flag = os.path.join(workdir, "fired")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def run_waybox(tag):
        log_path = os.path.join(workdir, f"waybox-{tag}.log")
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
        return proc, log, log_path, sock

    def stop(proc, log, tag):
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append(f"{tag}: waybox did not exit on SIGTERM")
        if proc.returncode not in (0, None):
            failures.append(f"{tag}: waybox exit status {proc.returncode}")
        log.close()

    def press_super_t(sock):
        subprocess.run(["wtype", "-M", "logo", "-k", "t", "-m", "logo"],
                       env=dict(env, WAYLAND_DISPLAY=sock), capture_output=True)
        time.sleep(0.5)

    logs = []
    # 1. Broken at startup.
    with open(rc, "w") as f:
        f.write(BROKEN.format(flag=flag))
    proc, log, log_path, sock = run_waybox("broken-start")
    logs.append(log_path)
    if sock is None:
        stop(proc, log, "broken-start")
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    time.sleep(0.3)
    text = open(log_path).read()
    expect(re.search(r"Configuration error: .*rc\.xml, line \d+: ", text) is not None,
           "a broken rc.xml at startup isn't reported with its line")
    expect("until it is fixed" in text, "no word on what Waybox uses instead")
    expect(proc.poll() is None, "waybox didn't start with a broken rc.xml")
    stop(proc, log, "broken-start")

    # 2. Good, then broken on reconfigure.
    with open(rc, "w") as f:
        f.write(GOOD.format(flag=flag))
    proc, log, log_path, sock = run_waybox("reconfigure")
    logs.append(log_path)
    try:
        time.sleep(0.3)
        press_super_t(sock)
        expect(os.path.exists(flag), "the key binding doesn't work with a good rc.xml")
        if os.path.exists(flag):
            os.remove(flag)
        with open(rc, "w") as f:
            f.write(BROKEN.format(flag=flag))
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        log.flush()
        expect("Keeping the current settings" in open(log_path).read(),
               "reconfiguring with a broken rc.xml doesn't say it keeps the settings")
        press_super_t(sock)
        expect(os.path.exists(flag),
               "the key binding was lost when reconfiguring with a broken rc.xml")
        expect(proc.poll() is None, "waybox exited")
    finally:
        stop(proc, log, "reconfigure")

    # 3. Empty: no settings, no error.
    open(rc, "w").close()
    proc, log, log_path, sock = run_waybox("empty")
    logs.append(log_path)
    time.sleep(0.3)
    expect("Configuration error" not in open(log_path).read(),
           "an empty rc.xml is reported as an error")
    stop(proc, log, "empty")

    contents = "".join(open(p).read() for p in logs)
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration config errors: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
