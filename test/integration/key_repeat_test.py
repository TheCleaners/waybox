#!/usr/bin/env python3
"""Headless integration test: a keybind's repeat attribute.

F5 says repeat="yes": held 1.5 s, its action runs again and again (after
the keyboard's repeat delay, at its rate), and stops when the key is let
go. F6 says nothing and F7 repeat="no": held as long, each runs once (as
Openbox; a stuck key doesn't run a binding over and over). F5 held while
another key is pressed: the repeat stops there, not at the release.

Skips (exit 77) when wtype or a headless display are unavailable.

Usage: key_repeat_test.py <waybox-binary>
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


def count(path):
    try:
        with open(path) as f:
            return len(f.read().split())
    except FileNotFoundError:
        return 0


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-kr-")
    files = {k: os.path.join(workdir, k) for k in ("yes", "default", "no")}
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)

    def bind(key, attr, name):
        return (f'    <keybind key="{key}"{attr}>\n'
                f'      <action name="Execute"><command>sh -c "echo x &gt;&gt; {files[name]}"'
                f'</command></action>\n'
                f'    </keybind>\n')
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<openbox_config xmlns="http://openbox.org/3.4/rc">\n  <keyboard>\n'
                + bind("F5", ' repeat="yes"', "yes") + bind("F6", "", "default")
                + bind("F7", ' repeat="no"', "no")
                + '  </keyboard>\n</openbox_config>\n')

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
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)

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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    failures = []
    rc = 0
    try:
        time.sleep(0.5)

        def hold(key, ms, extra=()):
            subprocess.run(["wtype", "-P", key, "-s", str(ms)] + list(extra) + ["-p", key],
                           env=cenv, capture_output=True)
            time.sleep(0.5)  # the last runs' shells finish

        hold("F5", 1500)
        held = count(files["yes"])
        time.sleep(0.8)
        after = count(files["yes"])
        # 600 ms, then 25 a second: about 23 in 1.5 s.
        if held < 8:
            failures.append(f'repeat="yes" held 1.5 s ran {held} times, not many')
        if after != held:
            failures.append(f'repeat="yes" went on after the release: {held} then {after}')

        hold("F6", 1500)
        hold("F7", 1500)
        for name in ("default", "no"):
            if count(files[name]) != 1:
                failures.append(f"{name}: held 1.5 s ran {count(files[name])} times, not once")

        # Held 1 s, then another key, held 0.8 s more: stopped at the other
        # key (about 11 runs), not at the release (about 31).
        os.remove(files["yes"])
        hold("F5", 1000, ["-k", "F8", "-s", "800"])
        n = count(files["yes"])
        if not 3 <= n <= 17:
            failures.append(f"another key didn't stop the repeat: {n} runs")
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
        failures.append("sanitizer/assert output")
        sys.stderr.write(contents)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration key_repeat: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
