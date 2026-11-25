#!/usr/bin/env python3
"""Integration test for tearing control (<waybox><tearing allow="yes"/>).

A fullscreen "game" asks for tearing (wp_tearing_control_v1, async). It
asks for fullscreen before its first frame, as games do; it must open
fullscreen. Then:
1. allowed in the settings: tearing is wanted on its output;
2. it switches to vsync: no longer wanted; back to async: wanted again;
3. it leaves fullscreen: not wanted (only fullscreen windows tear);
4. not allowed in the settings: never wanted.

(The headless backend can't flip without vsync; "wanted" is the decision,
which falls back to synced flips there, as on hardware that can't.)

Usage: tearing_test.py <waybox> <tearing_client>
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
    waybox, client_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-tearing-", dir="/tmp")
    failures = []

    def logtext_of(tag):
        return open(os.path.join(workdir, f"waybox-{tag}.log")).read()

    send_env = {}

    def cenv_of(_):
        return send_env["env"]

    def session(tag, allow, steps):
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-1"><action name="GoToDesktop"><to>1</to></action></keybind>
    <keybind key="W-2"><action name="GoToDesktop"><to>2</to></action></keybind>
  </keyboard>
  <waybox><virtualInput allow="always"/>{'<tearing allow="yes"/>' if allow else ''}</waybox>
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
        client = subprocess.Popen([client_bin], env=dict(env, WAYLAND_DISPLAY=sock),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in client.stdout],
                         daemon=True).start()

        def logtext():
            log.flush()
            return open(lp).read()

        def wanted():
            found = re.findall(r"wb-tearing-wanted \S+ (\d)", logtext())
            return found[-1] if found else "0"

        send_env["env"] = dict(env, WAYLAND_DISPLAY=sock)

        def send(cmd):
            client.stdin.write(cmd + "\n")
            client.stdin.flush()
            time.sleep(0.6)

        try:
            if lines.get(timeout=5) != "configured":
                raise RuntimeError("the client didn't show")
            time.sleep(0.8)
            steps(send, wanted)
        except (RuntimeError, queue.Empty) as e:
            failures.append(f"{tag}: {e or 'the client did not show'}")
        finally:
            client.kill()
            time.sleep(0.3)
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

    def allowed(send, wanted):
        # It asked for fullscreen before its first frame, as games do.
        if not re.search(r"wb-geom fullscreen-on \S+ tearing-client", logtext_of("allowed")):
            failures.append("a window that asked for fullscreen before showing opened windowed")
        if wanted() != "1":
            failures.append("a fullscreen window asking for tearing didn't get it")
        send("vsync")
        if wanted() != "0":
            failures.append("switching to vsync didn't end tearing")
        send("async")
        if wanted() != "1":
            failures.append("switching back to async didn't bring tearing back")
        if shutil.which("wtype"):
            # On another desktop (hidden): what's shown mustn't tear.
            subprocess.run(["wtype", "-M", "logo", "2", "-m", "logo"],
                           env=cenv_of(send), capture_output=True, timeout=5)
            time.sleep(0.6)
            if wanted() != "0":
                failures.append("a fullscreen game on another desktop still makes the output tear")
            subprocess.run(["wtype", "-M", "logo", "1", "-m", "logo"],
                           env=cenv_of(send), capture_output=True, timeout=5)
            time.sleep(0.6)
            if wanted() != "1":
                failures.append("back on the game's desktop, it didn't tear again")
        send("windowed")
        if wanted() != "0":
            failures.append("a window that left fullscreen still tears")
    session("allowed", True, allowed)

    def not_allowed(send, wanted):
        if wanted() != "0":
            failures.append("tearing without <tearing allow=\"yes\"/>")
    session("default", False, not_allowed)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration tearing: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
