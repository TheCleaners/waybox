#!/usr/bin/env python3
"""Integration test: Exit asks first (Openbox's <prompt>, on by default).

Super+Q runs Exit; Super+Shift+Q runs Exit with <prompt>no</prompt>.

1. Super+Q: the question is shown; Escape: Waybox goes on.
2. Super+Q, Return (Cancel is chosen first): Waybox goes on.
3. Super+Q, a click outside the card: Waybox goes on (if wlrctl is there).
4. Super+Q, Right (Exit chosen), Return: Waybox exits.
5. Again, Super+Shift+Q (<prompt>no</prompt>): Waybox exits, unasked.
6. Again, waybox-msg action Exit prompt=no (a logout script's): exits,
   unasked.

Skips (exit 77) without wtype, or a headless display.

Usage: exit_prompt_test.py <waybox> <waybox-msg>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="W-q"><action name="Exit"/></keybind>
    <keybind key="W-S-q"><action name="Exit"><prompt>no</prompt></action></keybind>
  </keyboard>
</openbox_config>
"""


def start(waybox, workdir, tag):
    cfg = os.path.join(workdir, f"cfg-{tag}")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, f"run-{tag}")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, f"waybox-{tag}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            return proc, log, log_path, dict(env, WAYLAND_DISPLAY=socks[0])
    log.close()
    proc.kill()
    return None, None, log_path, None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, msg = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-exit-", dir="/tmp")
    failures = []

    proc, log, log_path, cenv = start(waybox, workdir, "ask")
    if proc is None:
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)

    def text():
        log.flush()
        return open(log_path).read()

    def keys(*args):
        subprocess.run(["wtype", *args], env=cenv, capture_output=True, timeout=10)
        time.sleep(0.6)

    def ask():
        before = text().count("wb-confirm open")
        keys("-M", "logo", "-k", "q", "-m", "logo")
        return text().count("wb-confirm open") > before

    try:
        # 1. Escape.
        if not ask():
            failures.append("Exit didn't ask first")
        keys("-k", "Escape")
        if proc.poll() is not None or "wb-confirm no" not in text():
            failures.append("Escape didn't cancel (or Waybox exited)")
        # 2. Return: Cancel is chosen first.
        ask()
        keys("-k", "Return")
        if proc.poll() is not None or text().count("wb-confirm no") < 2:
            failures.append("Return (Cancel chosen first) didn't cancel")
        # 3. A click outside the card.
        if shutil.which("wlrctl"):
            ask()
            subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                           capture_output=True)
            subprocess.run(["wlrctl", "pointer", "move", "20", "20"], env=cenv,
                           capture_output=True)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
            time.sleep(0.6)
            if proc.poll() is not None or text().count("wb-confirm no") < 3:
                failures.append("a click outside the card didn't cancel")
        # 4. Right, Return: Exit.
        ask()
        keys("-k", "Right")
        keys("-k", "Return")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            failures.append("Exit chosen didn't exit")
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()

    # 5. <prompt>no</prompt>: at once.
    proc, log, log_path, cenv = start(waybox, workdir, "now")
    if proc is not None:
        try:
            keys("-M", "logo", "-M", "shift", "-k", "q", "-m", "shift", "-m", "logo")
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                failures.append("<prompt>no</prompt> didn't exit at once")
                proc.kill()
            if "wb-confirm open" in open(log_path).read():
                failures.append("<prompt>no</prompt> asked anyway")
        finally:
            if proc.poll() is None:
                proc.kill()
            log.close()

    # 6. Over IPC, prompt=no: at once.
    proc, log, log_path, cenv = start(waybox, workdir, "ipc")
    if proc is not None:
        try:
            time.sleep(0.5)
            subprocess.run([msg, "action", "Exit", "prompt=no"], env=cenv, capture_output=True,
                           timeout=10)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                failures.append("waybox-msg action Exit prompt=no didn't exit at once")
                proc.kill()
            if "wb-confirm open" in open(log_path).read():
                failures.append("waybox-msg action Exit prompt=no asked anyway")
        finally:
            if proc.poll() is None:
                proc.kill()
            log.close()

    for name in os.listdir(workdir):
        if name.endswith(".log") and any(s in open(os.path.join(workdir, name)).read()
                                         for s in ("Sanitizer", "runtime error")):
            failures.append(f"sanitizer output in {name}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration exit_prompt: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
