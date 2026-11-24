#!/usr/bin/env python3
"""Integration test for Openbox pipe menus in the native root menu.

menu.xml's root menu has two pipe submenus:
  - "Pipe": a script that answers after 0.5s with an <openbox_pipe_menu>
    whose entry touches a marker file. Opening it shows a placeholder, the
    output replaces it, and Down+Return must run the piped entry.
  - "Slow": a script that never answers. While it runs the compositor must
    stay responsive (wlr-randr answers quickly), and after the 5s timeout the
    script must be gone (killed and reaped, no zombie).

Skips (exit 77) without wlrctl, wtype, wlr-randr or a headless display.

Usage: pipe_menu_test.py <waybox-binary>
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
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wlrctl", "wtype", "wlr-randr"):
        need(tool)

    workdir = tempfile.mkdtemp(prefix="waybox-pipe-", dir="/tmp")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg)
    marker = os.path.join(workdir, "piped-entry-ran")
    pipe_script = os.path.join(workdir, "pipe.sh")
    slow_script = os.path.join(workdir, "slow.sh")
    with open(pipe_script, "w") as f:
        f.write(f"""#!/bin/sh
sleep 0.5
cat <<EOF
<openbox_pipe_menu>
  <item label="Piped"><action name="Execute"><execute>touch {marker}</execute></action></item>
</openbox_pipe_menu>
EOF
""")
    with open(slow_script, "w") as f:
        f.write("#!/bin/sh\nexec sleep 300\n")
    os.chmod(pipe_script, 0o755)
    os.chmod(slow_script, 0o755)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse>
    <context name="Root">
      <mousebind button="Right" action="Press">
        <action name="ShowMenu"><menu>root-menu</menu></action>
      </mousebind>
    </context>
  </mouse>
</openbox_config>
""")
    with open(os.path.join(cfg, "menu.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Root">
    <menu id="pipe" label="Pipe" execute="{pipe_script}"/>
    <menu id="slow" label="Slow" execute="{slow_script}"/>
  </menu>
</openbox_menu>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=workdir, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)

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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []

    def run(*args):
        subprocess.run(list(args), env=cenv, capture_output=True)

    def key(name):
        run("wtype", "-k", name)
        time.sleep(0.3)  # generous: sanitizer builds react slowly

    def open_root_menu():
        run("wlrctl", "pointer", "move", "-5000", "-5000")
        run("wlrctl", "pointer", "move", "200", "200")
        time.sleep(0.1)
        run("wlrctl", "pointer", "click", "right")
        time.sleep(0.4)

    def children_running(script):
        out = subprocess.run(["pgrep", "-P", str(proc.pid), "-a"],
                             capture_output=True, text=True).stdout
        return script in out

    try:
        # 1. Pipe menu: placeholder, then output; run the piped entry.
        open_root_menu()
        key("Down")      # "Pipe"
        key("Right")     # open it: starts the script, shows "Loading…"
        time.sleep(1.2)  # the script answers after 0.5s
        key("Down")      # first entry of the (now loaded) submenu
        key("Return")
        for _ in range(20):
            if os.path.exists(marker):
                break
            time.sleep(0.1)
        if not os.path.exists(marker):
            failures.append("the piped entry did not run (placeholder not replaced?)")

        # 2. A pipe menu that never answers must not block the compositor.
        open_root_menu()
        key("Down")
        key("Down")      # "Slow"
        key("Right")
        for _ in range(30):
            if children_running(slow_script):
                break
            time.sleep(0.1)
        else:
            failures.append("the slow pipe menu's command was not started")
        t0 = time.time()
        r = subprocess.run(["wlr-randr"], env=cenv, capture_output=True,
                           timeout=10)
        if r.returncode != 0 or time.time() - t0 > 1.0:
            failures.append(f"compositor unresponsive while a pipe menu runs "
                            f"({time.time() - t0:.1f}s)")
        time.sleep(5.5)  # past the 5s timeout
        if children_running(slow_script):
            failures.append("the slow pipe menu's command is still running after the timeout")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
        key("Escape")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        log.close()

    contents = open(log_path).read()
    if "pipe menu slow: no complete output after 5s" not in contents:
        failures.append("no timeout logged for the slow pipe menu")
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration pipe menu: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
