#!/usr/bin/env python3
"""Headless integration test: drive Waybox with synthesized input.

Exercises the real input path end to end — the virtual-keyboard protocol holds
Alt and taps Tab (Alt+Tab -> NextWindow), which opens the interactive task
switcher OSD; releasing Alt commits the selection. This runs through the action
framework, the switcher grab/OSD render, and the focus/stacking code under
AddressSanitizer.

Designed to run from `meson test`. It SKIPS (exit 77) when its runtime
dependencies are unavailable (no foot/wtype, or the headless backend cannot
bring up a Wayland display), so CI without a compositor/clients stays green;
locally, with a sanitized build and the tools installed, it actually runs.

Usage: alt_tab_test.py <waybox-binary> <rc.xml>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def need(tool):
    path = shutil.which(tool)
    if path is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)
    return path


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, rc_xml = sys.argv[1], sys.argv[2]
    need("foot")
    need("wtype")

    workdir = tempfile.mkdtemp(prefix="waybox-it-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    shutil.copyfile(rc_xml, os.path.join(cfg, "rc.xml"))

    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        # Fail hard on any sanitizer finding.
        ASAN_OPTIONS="detect_leaks=1:exitcode=99",
    )
    # A private runtime dir so we don't collide with a real session.
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    env["XDG_RUNTIME_DIR"] = runtime

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)

    # Wait for the compositor socket to appear.
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

    def client(cmd):
        return subprocess.Popen(cmd, env=cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)

    rc = 0
    try:
        a = client(["foot"])
        time.sleep(1.5)
        b = client(["foot"])
        time.sleep(2.0)

        # Drive Alt+Tab through the virtual keyboard a few times.
        for _ in range(3):
            r = subprocess.run(["wtype", "-M", "alt", "-k", "Tab", "-m", "alt"],
                               env=cenv, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"wtype failed: {r.stderr.strip()}", file=sys.stderr)
                rc = 1
            time.sleep(0.6)

        time.sleep(0.5)
        a.terminate()
        time.sleep(1.0)
        b.terminate()
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
    bad = ("Sanitizer", "runtime error", "AddressSanitizer", "LeakSanitizer",
           "Assertion", "use-after-free")
    if any(marker in contents for marker in bad):
        print("FAIL: sanitizer/assert output detected", file=sys.stderr)
        sys.stderr.write(contents)
        rc = 1
    if "Display destroyed" not in contents:
        print("FAIL: no clean shutdown ('Display destroyed') in log", file=sys.stderr)
        sys.stderr.write(contents)
        rc = 1
    # Expect focus to have moved at least once via the keybinding.
    if contents.count("Keyboard focus is now") < 3:
        print("FAIL: expected multiple focus changes from Alt+Tab", file=sys.stderr)
        sys.stderr.write(contents)
        rc = 1

    if rc == 0:
        shutil.rmtree(workdir, ignore_errors=True)
        print("integration alt_tab: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
