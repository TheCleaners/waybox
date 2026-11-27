#!/usr/bin/env python3
"""Headless integration test: several keys, buttons or contexts in one
binding (as Openbox reads them: each an alternative, running the same).

key="F5 F6": either runs it. key="F7 F8" with a chain under it: F8 then x
runs the chain's action. <context name="Bogus Root"> with a
<mousebind button="Middle Right">: the unknown context skipped, the rest
kept; a right click and a middle click on the desktop each run it.

Skips (exit 77) when wtype, wlrctl or a headless display are unavailable.

Usage: grouped_binds_test.py <waybox-binary>
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


def lines(path):
    try:
        with open(path) as f:
            return f.read().split()
    except FileNotFoundError:
        return []


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wtype", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-gb-")
    out = os.path.join(workdir, "ran")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)

    def run(word):
        return (f'<action name="Execute"><command>sh -c "echo {word} &gt;&gt; {out}"'
                f'</command></action>')
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <keybind key="F5 F6">{run("key")}</keybind>
    <keybind key="F7 F8">
      <keybind key="x">{run("chain")}</keybind>
    </keybind>
  </keyboard>
  <mouse>
    <context name="Bogus Root">
      <mousebind button="Middle Right" action="Press">{run("mouse")}</mousebind>
    </context>
  </mouse>
</openbox_config>
""")

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
    rc = 0
    got = {}
    try:
        time.sleep(0.5)

        def step(name, *cmds):
            for c in cmds:
                subprocess.run(c, env=cenv, capture_output=True)
                time.sleep(0.3)
            time.sleep(0.5)
            got[name] = lines(out)

        step("F6", ["wtype", "-k", "F6"])
        step("F5", ["wtype", "-k", "F5"])
        step("F8 x", ["wtype", "-k", "F8"], ["wtype", "-k", "x"])
        step("right", ["wlrctl", "pointer", "move", "100", "100"],
             ["wlrctl", "pointer", "click", "right"])
        step("middle", ["wlrctl", "pointer", "click", "middle"])
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    failures = []
    want = {
        "F6": ["key"],
        "F5": ["key", "key"],
        "F8 x": ["key", "key", "chain"],
        "right": ["key", "key", "chain", "mouse"],
        "middle": ["key", "key", "chain", "mouse", "mouse"],
    }
    for name, w in want.items():
        if got.get(name) != w:
            failures.append(f"after {name}: ran {got.get(name)}, not {w}")
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
    print("integration grouped_binds: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
