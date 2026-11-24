#!/usr/bin/env python3
"""Integration test for the built-in <waybox><idle screenOff="seconds"/>.

1. With screenOff="1": the screen switches off after 1 s without input and
   back on when the pointer moves.
2. An idle inhibitor (a video playing) keeps it on.
3. Without <idle> it never switches off.

Skips (exit 77) without wlrctl, or a headless display.

Usage: idle_screen_off_test.py <waybox> <inhibit_client>
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, inhibit_bin = sys.argv[1:]
    if shutil.which("wlrctl") is None:
        print("SKIP: wlrctl not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-idleoff-", dir="/tmp")
    failures = []

    def session(tag, idle, steps):
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{idle}</waybox>
</openbox_config>
""")
        runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
                   WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        lp = os.path.join(workdir, f"waybox-{tag}.log")
        log = open(lp, "w")
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

        def logtext():
            log.flush()
            return open(lp).read()
        try:
            steps(cenv, logtext)
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"{tag}: waybox exit status {proc.returncode}")
            if any(s in open(lp).read() for s in ("Sanitizer", "Assertion", "runtime error")):
                failures.append(f"{tag}: sanitizer/assert output")

    def offs(t):
        return len(re.findall(r"wb-output-power \S+ off", t))

    # 1. Off after 1 s, on with input.
    def basic(cenv, logtext):
        time.sleep(1.8)
        if offs(logtext()) != 1:
            failures.append("the screen didn't switch off after 1 s idle")
            return
        subprocess.run(["wlrctl", "pointer", "move", "5", "5"], env=cenv,
                       capture_output=True, timeout=5)
        time.sleep(0.4)
        if not re.search(r"wb-output-power \S+ on", logtext()):
            failures.append("moving the pointer didn't switch the screen back on")
    session("basic", '<idle screenOff="1"/>', basic)

    # 2. An inhibitor keeps it on.
    def inhibited(cenv, logtext):
        c = subprocess.Popen([inhibit_bin], env=cenv, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in c.stdout],
                         daemon=True).start()
        try:
            lines.get(timeout=5)
            c.stdin.write("new\n")
            c.stdin.flush()
            lines.get(timeout=5)
            time.sleep(2.2)
            if offs(logtext()):
                failures.append("the screen switched off while idle was inhibited")
        except queue.Empty:
            failures.append("the inhibit client didn't answer")
        finally:
            c.kill()
    session("inhibited", '<idle screenOff="1"/>', inhibited)

    # 3. Not configured: never.
    def unset(cenv, logtext):
        time.sleep(2.0)
        if offs(logtext()):
            failures.append("the screen switched off without an <idle> setting")
    session("unset", "", unset)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration idle screen off: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
