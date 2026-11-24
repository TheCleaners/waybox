#!/usr/bin/env python3
"""Integration test for keyboard-shortcuts-inhibit.

A program (a VM viewer) asks for the keyboard shortcuts once focused, and
rc.xml binds Super+K to write a file:
1. its inhibitor is active, and Super+K goes to the program, not the
   binding;
2. Super+Escape takes the shortcuts back: inactive, and Super+K runs the
   binding again;
3. Super+Escape again gives them back to it: active;
4. with <shortcutsInhibit allow="no"/>, it never gets them.

Skips (exit 77) without wtype, or a headless display.

Usage: shortcuts_inhibit_test.py <waybox> <kbinhibit_client>
"""
import os
import queue
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
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-kbinhibit-", dir="/tmp")
    failures = []

    def session(tag, allow, steps):
        marker = os.path.join(workdir, f"bound-{tag}")
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-k"><action name="Execute"><command>touch {marker}</command></action></keybind>
  </keyboard>
  <waybox><virtualInput allow="always"/>{'' if allow else '<shortcutsInhibit allow="no"/>'}</waybox>
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
        client = subprocess.Popen([client_bin], env=cenv, stdout=subprocess.PIPE, text=True)
        lines = queue.Queue()
        seen = []
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in client.stdout],
                         daemon=True).start()

        def drain(t=0.6):
            end = time.time() + t
            new = []
            while time.time() < end:
                try:
                    new.append(lines.get(timeout=max(0.01, end - time.time())))
                except queue.Empty:
                    break
            seen.extend(new)
            return new

        def keys(*args):
            subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
            return drain()

        def state():
            states = [l for l in seen if l in ("active", "inactive")]
            return states[-1] if states else "none"

        try:
            drain(1.5)
            if "configured" not in seen:
                raise RuntimeError("the program didn't show")
            steps(keys, state, marker)
        except RuntimeError as e:
            failures.append(f"{tag}: {e}")
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

    super_k = ["-M", "logo", "k", "-m", "logo"]
    super_esc = ["-M", "logo", "-k", "Escape", "-m", "logo"]

    def allowed(keys, state, marker):
        if state() != "active":
            failures.append(f"the focused program didn't get the shortcuts ({state()})")
        got = keys(*super_k)
        if os.path.exists(marker):
            failures.append("Super+K ran Waybox's binding while the program had the shortcuts")
        if "key" not in got:
            failures.append("Super+K didn't reach the program")
        keys(*super_esc)
        if state() != "inactive":
            failures.append("Super+Escape didn't take the shortcuts back")
        keys(*super_k)
        time.sleep(0.4)
        if not os.path.exists(marker):
            failures.append("after Super+Escape, Super+K didn't run the binding")
        keys(*super_esc)
        if state() != "active":
            failures.append("a second Super+Escape didn't give the shortcuts back")
    session("allowed", True, allowed)

    def refused(keys, state, marker):
        if state() == "active":
            failures.append("shortcuts taken with <shortcutsInhibit allow=\"no\"/>")
        keys(*super_k)
        time.sleep(0.4)
        if not os.path.exists(marker):
            failures.append("the binding didn't run with inhibiting turned off")
    session("refused", False, refused)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration shortcuts inhibit: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
