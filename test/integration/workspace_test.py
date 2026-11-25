#!/usr/bin/env python3
"""Integration test for ext-workspace-v1 (desktops shown in panels).

workspace_client stands in for a panel: it prints the workspace list after
every "done" and activates workspaces on request. With 3 desktops (the
first named "web") on 2 headless outputs:

1. A new client sees the group on both outputs and all three desktops,
   desktop 1 active.
2. A key binding switches to desktop 2: the client is told.
3. The client activates desktop 3: Waybox switches there.
4. Reconfiguring to 2 renamed desktops removes the third and moves the
   active one; the client sees the new names.
5. A second client that connects now gets the current state.
6. stop is answered with finished.

Skips (exit 77) without wtype or a headless display.

Usage: workspace_test.py <waybox-binary> <workspace_client-binary>
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
import wbtest

SKIP = 77


def rc_xml(number, names):
    names_xml = "".join(f"<name>{n}</name>" for n in names)
    keys = "".join(
        f'<keybind key="W-{n}"><action name="GoToDesktop"><to>{n}</to></action></keybind>'
        for n in (1, 2, 3))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>{number}</number><names>{names_xml}</names></desktops>
  <keyboard>{keys}</keyboard>
</openbox_config>
"""


class Client:
    def __init__(self, binary, env):
        self.proc = subprocess.Popen([binary], env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def wait_for(self, expected, timeout=3.0):
        """The first line equal to `expected` within the timeout, else the
        last line seen (or None)."""
        last = None
        end = time.time() + timeout
        while time.time() < end:
            try:
                last = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if last == expected:
                return last
        return last

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1], sys.argv[2]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-ws-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(rc_xml(3, ["web"]))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime,
               WB_RC_XML="", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
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
    time.sleep(0.3)  # both outputs up

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    clients = []

    def expect(client, line, what):
        got = client.wait_for(line)
        if got != line:
            failures.append(f"{what}: expected {line!r}, last got {got!r}")

    try:
        panel = Client(client_bin, cenv)
        clients.append(panel)
        expect(panel, "done outputs=2 1:web:active 2:Desktop 2:- 3:Desktop 3:-",
               "initial state")

        subprocess.run(["wtype", "-M", "logo", "-k", "2", "-m", "logo"],
                       env=cenv, capture_output=True)
        expect(panel, "done outputs=2 1:web:- 2:Desktop 2:active 3:Desktop 3:-",
               "switch by key binding")

        panel.send("activate 3")
        expect(panel, "done outputs=2 1:web:- 2:Desktop 2:- 3:Desktop 3:active",
               "activate from the panel")
        if "wb-desktop 3" not in open(log_path).read():
            failures.append("activate did not switch Waybox to desktop 3")

        with open(rc_path, "w") as f:
            f.write(rc_xml(2, ["a", "b"]))
        proc.send_signal(signal.SIGUSR2)
        expect(panel, "done outputs=2 1:a:- 2:b:active", "reconfigure to 2 desktops")

        late = Client(client_bin, cenv)
        clients.append(late)
        expect(late, "done outputs=2 1:a:- 2:b:active", "state for a new client")

        panel.send("stop")
        expect(panel, "finished", "stop")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        for c in clients:
            c.close()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        log.close()

    contents = open(log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration workspace: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
