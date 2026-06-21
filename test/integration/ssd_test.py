#!/usr/bin/env python3
"""Headless integration test for server-side decoration (titlebar) input.

Forces a server-side frame on a foot window via an app rule, places it at a
known geometry, then exercises the frame's input paths under ASan:
  - clicks the titlebar (drag-to-move grab begins; no crash), and
  - clicks the close button, which sends xdg close so foot exits.
foot exiting proves the frame hit-test (toplevel_frame_at) resolved the close
button and ran its action. Skips (exit 77) without wlrctl/foot/headless.

Usage: ssd_test.py <waybox-binary>
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
    need("wlrctl")
    need("foot")

    workdir = tempfile.mkdtemp(prefix="waybox-ssd-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    # Force server-side decorations on every window, placed at a fixed spot so
    # the titlebar/close-button coordinates are known.
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <application class="*">
      <decor>yes</decor>
      <position><x>100</x><y>100</y></position>
      <size><width>400</width><height>250</height></size>
    </application>
  </applications>
</openbox_config>
""")

    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    suppfile = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "lsan.supp")
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        ASAN_OPTIONS="detect_leaks=1:exitcode=99",
        LSAN_OPTIONS=f"suppressions={suppfile}:print_suppressions=0",
    )

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)

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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=display)
    foot = subprocess.Popen(["foot", "sleep", "30"], env=cenv,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def move_to(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"],
                       env=cenv, capture_output=True)
        time.sleep(0.03)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)],
                       env=cenv, capture_output=True)
        time.sleep(0.05)

    rc = 0
    try:
        time.sleep(1.5)  # let foot map and the frame appear
        if foot.poll() is not None:
            print("FAIL: foot exited before interaction", file=sys.stderr)
            rc = 1

        # Click the titlebar (a left grab begins; releasing ends it). The
        # window is at (100,100); the titlebar sits just above the client.
        move_to(160, 90)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        time.sleep(0.2)
        if foot.poll() is not None:
            print("FAIL: foot exited after a titlebar click (should not)",
                  file=sys.stderr)
            rc = 1

        # Click the close button (top-right of the titlebar) -> foot exits.
        move_to(484, 87)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)
        try:
            foot.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print("FAIL: close button did not close the window", file=sys.stderr)
            rc = 1
    finally:
        if foot.poll() is None:
            foot.terminate()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free", "heap-use-after-free")):
        print("FAIL: sanitizer/assert output", file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration ssd: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
