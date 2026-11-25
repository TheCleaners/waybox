#!/usr/bin/env python3
"""Integration test with real IBus (ibus-daemon + ibus-wayland), started by
Waybox from <waybox><inputMethod> as wbconf's "IBus (Wayland)" choice does.

1. Waybox starts IBus, and ibus-wayland connects as the input method.
2. A letter typed into a text field arrives as text IBus commits (without
   IBus in the path it would arrive as a plain key).

Isolated from the user's session: a private D-Bus, a temporary HOME and
in-memory settings. Skips (exit 77) without ibus-daemon, ibus-wayland,
dbus-daemon or wtype (CI), or a headless display.

Usage: ibus_real_test.py <waybox> <text_input_client>
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
IBUS_WAYLAND = "/usr/libexec/ibus-wayland"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, ti_bin = sys.argv[1:]
    for tool in ("ibus-daemon", "dbus-daemon", "wtype"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    if not os.access(IBUS_WAYLAND, os.X_OK):
        print(f"SKIP: {IBUS_WAYLAND} not found (ibus-wayland)", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-ibus-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(workdir, "config")
    os.makedirs(home)
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <virtualInput allow="always"/>
    <inputMethod command="sh -c 'ibus-daemon --replace --xim &amp; sleep 1; exec {IBUS_WAYLAND}'">
      <env name="XMODIFIERS" value="@im=ibus"/>
    </inputMethod>
  </waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    # Services D-Bus starts on it (portals, the a11y bus) inherit its output:
    # not the test's, or the test runner waits on them.
    bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           stdin=subprocess.DEVNULL, text=True)
    address = bus.stdout.readline().strip()
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, HOME=home,
               DBUS_SESSION_BUS_ADDRESS=address, GSETTINGS_BACKEND="memory",
               WB_RC_XML="", WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    for v in ("WAYLAND_DISPLAY", "DISPLAY", "XMODIFIERS", "GTK_IM_MODULE", "QT_IM_MODULE"):
        env.pop(v, None)
    lp = os.path.join(workdir, "waybox.log")
    log = open(lp, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    failures = []
    client = None

    def text():
        log.flush()
        return open(lp).read()

    try:
        sock = None
        for _ in range(50):
            time.sleep(0.1)
            socks = [n for n in os.listdir(runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                sock = socks[0]
                break
        if sock is None:
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            sys.exit(SKIP)
        for _ in range(60):
            if "Input method connected" in text():
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("ibus-wayland didn't connect as the input method")
        cenv = dict(env, WAYLAND_DISPLAY=sock)
        client = subprocess.Popen([ti_bin], env=cenv, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        lines = queue.Queue()
        seen = []
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in client.stdout],
                         daemon=True).start()

        def drain(t):
            end = time.time() + t
            while time.time() < end:
                try:
                    seen.append(lines.get(timeout=max(0.01, end - time.time())))
                except queue.Empty:
                    pass

        drain(2.0)
        if "ti-enter" not in seen:
            raise RuntimeError(f"the text field never got focus: {seen}")
        seen.clear()
        subprocess.run(["wtype", "x"], env=cenv, capture_output=True, timeout=10)
        drain(1.5)
        # Through IBus, a typed letter arrives as text IBus commits; without
        # it, as a plain key.
        if "commit x" not in seen:
            failures.append(f"the letter didn't come through IBus: {seen}")
    except RuntimeError as e:
        failures.append(str(e))
    finally:
        if client is not None and client.poll() is None:
            client.kill()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        bus.terminate()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(open(lp).read()[-2000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration real IBus: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
