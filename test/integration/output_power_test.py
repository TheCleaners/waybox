#!/usr/bin/env python3
"""Integration test: screens off and on for power saving (#76).

1. power_client off: the output is switched off, but stays in the layout
   (output-management clients still see it enabled) and keeps its windows:
   a new window still maps on it.
2. power_client on: it's back on and draws frames (grim captures it).
3. As in a session: swayidle runs power_client off after 1 s of idle and
   power_client on when the pointer moves.

Skips (exit 77) without foot, wlrctl, wlr-randr or grim, or a headless
display. Step 3 is skipped without swayidle.

Usage: output_power_test.py <waybox> <power_client>
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
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, power = sys.argv[1:]
    for tool in ("foot", "wlrctl", "wlr-randr", "grim"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-power-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=os.path.join(workdir, "config"),
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
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
    if sock is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    children = []

    def logtext():
        log.flush()
        return open(log_path).read()

    def run(*argv, timeout=10):
        try:
            return subprocess.run(list(argv), env=cenv, capture_output=True,
                                  text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def enabled_in_randr():
        r = run("wlr-randr")
        return r is not None and "Enabled: yes" in r.stdout

    def spawn(*argv):
        p = subprocess.Popen(list(argv), env=cenv, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        children.append(p)
        return p

    def wait_log(pattern, since, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, logtext()[since:]):
                return True
            time.sleep(0.1)
        return False

    try:
        spawn("foot", "-a", "before")
        if not wait_log(r"wb-geom mapped \S+ before", 0, 10):
            raise RuntimeError("the first window never mapped")

        # 1. Off.
        mark = len(logtext())
        r = run(power, "off")
        if r is None or r.returncode != 0:
            failures.append(f"power off wasn't confirmed: {r and r.stdout}")
        if not wait_log(r"wb-output-power \S+ off", mark):
            failures.append("the output wasn't switched off")
        if not enabled_in_randr():
            failures.append("a screen off for power saving shows as disabled to wlr-randr")
        spawn("foot", "-a", "while-off")
        if not wait_log(r"wb-geom mapped \d+x\d+\+\d+\+\d+ while-off", mark, 10):
            failures.append("a window opened while the screen was off didn't map on it")

        # 2. On.
        mark = len(logtext())
        r = run(power, "on")
        if r is None or r.returncode != 0:
            failures.append(f"power on wasn't confirmed: {r and r.stdout}")
        if not wait_log(r"wb-output-power \S+ on", mark):
            failures.append("the output wasn't switched back on")
        time.sleep(0.3)
        shot = os.path.join(workdir, "after.png")
        r = run("grim", shot)
        if r is None or r.returncode != 0 or not os.path.exists(shot):
            failures.append("the output didn't draw after being switched on")

        # 3. swayidle, as in a session.
        if shutil.which("swayidle"):
            mark = len(logtext())
            spawn("swayidle", "-w", "timeout", "1", f"{power} off",
                  "resume", f"{power} on")
            if not wait_log(r"wb-output-power \S+ off", mark, 6):
                failures.append("swayidle's timeout didn't switch the screen off")
            mark = len(logtext())
            run("wlrctl", "pointer", "move", "5", "5")
            if not wait_log(r"wb-output-power \S+ on", mark, 5):
                failures.append("moving the pointer didn't switch the screen back on")
        else:
            print("note: swayidle not found, step 3 skipped", file=sys.stderr)
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        for c in children:
            if c.poll() is None:
                c.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()

    contents = open(log_path).read()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration output power: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
