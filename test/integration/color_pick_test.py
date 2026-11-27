#!/usr/bin/env python3
"""Integration test: picking a color from the screen (IPC pick_color).

A headless Waybox whose background is red on the left half and blue on
the right (desktop_client background halves).
1. A pick, a left click on the left half: "#ff0000"; on the right half:
   "#0000ff". The cursor is solid green (its own theme, hotspot included),
   and headless draws it into frames: it must not be what's read.
2. The pick's click goes nowhere else: a Desktop mouse binding doesn't run
   (and does, for a click with no pick on).
3. Escape cancels; so does a right click.
4. The output turned 90 degrees, at scale 2: still red on the left and
   blue on the right (the point is turned into the buffer's).
5. The asking client goes mid-pick: the pick ends, and another can start.
Skips (77) without wlrctl, wtype or wlr-randr, or a headless display.

Usage: color_pick_test.py <waybox> <waybox-msg> <desktop_client>
"""
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77


def xcursor(size, argb):
    """A solid square cursor (Xcursor file), hotspot in its middle."""
    hot = size // 2
    header = struct.pack("<4sIII", b"Xcur", 16, 0x10000, 1)
    toc = struct.pack("<III", 0xfffd0002, size, 16 + 12)
    chunk = struct.pack("<IIIIIIIII", 36, 0xfffd0002, size, 1, size, size, hot, hot, 50)
    return header + toc + chunk + struct.pack("<I", argb) * (size * size)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, msg, desktop = sys.argv[1:]
    for tool in ("wlrctl", "wtype", "wlr-randr"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-pick-", dir="/tmp")
    # A solid green cursor (its hotspot too): if it were in the frame read,
    # picks would come out green.
    cursors = os.path.join(workdir, "icons", "Green", "cursors")
    os.makedirs(cursors)
    for name in ("default", "left_ptr", "crosshair", "cross"):
        with open(os.path.join(cursors, name), "wb") as f:
            f.write(xcursor(24, 0xff00ff00))
    marker = os.path.join(workdir, "clicked")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse><context name="Desktop"><mousebind button="Left" action="Press">
    <action name="Execute"><execute>touch {marker}</execute></action>
  </mousebind></context></mouse>
  <waybox><ipc enabled="yes"/><virtualInput allow="always"/></waybox>
</openbox_config>
""")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
               XCURSOR_PATH=os.path.join(workdir, "icons"), XCURSOR_THEME="Green",
               XCURSOR_SIZE="24",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
               + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    bg = subprocess.Popen([desktop, "background", "halves"], env=cenv,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wbtest.reap_on_exit(bg)
    time.sleep(0.8)

    def size():
        out = subprocess.run([msg, "get_outputs"], env=cenv, capture_output=True, text=True)
        o = json.loads(out.stdout)["outputs"][0]
        return o["width"], o["height"]

    def move(x, y):
        subprocess.run(["wlrctl", "pointer", "move", "-9000", "-9000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=cenv,
                       capture_output=True)
        time.sleep(0.2)

    def start_pick():
        p = subprocess.Popen([msg, "pick_color"], env=cenv, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(p)
        time.sleep(0.4)
        return p

    def answer(p):
        try:
            out, err = p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            return "(no answer)"
        try:
            return json.loads(out).get("color", out)
        except ValueError:
            return (out + err).strip()

    def pick_at(x, y):
        move(x, y)
        p = start_pick()
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        return answer(p)

    try:
        w, h = size()
        # 1, 2.
        for x, want in ((w // 4, "#ff0000"), (3 * w // 4, "#0000ff")):
            got = pick_at(x, h // 2)
            if got != want:
                failures.append(f"pick at x={x}: {got!r}, not {want}")
        time.sleep(0.3)
        if os.path.exists(marker):
            failures.append("a pick's click reached a mouse binding")
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv, capture_output=True)
        time.sleep(0.4)
        if not os.path.exists(marker):
            failures.append("a click with no pick on didn't reach the binding (test broken?)")
        # 3.
        p = start_pick()
        subprocess.run(["wtype", "-k", "Escape"], env=cenv, capture_output=True)
        got = answer(p)
        if "cancelled" not in got:
            failures.append(f"Escape: {got!r}")
        p = start_pick()
        subprocess.run(["wlrctl", "pointer", "click", "right"], env=cenv, capture_output=True)
        got = answer(p)
        if "cancelled" not in got:
            failures.append(f"a right click: {got!r}")
        # 4. Turned and scaled.
        subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--transform", "90",
                        "--scale", "2"], env=cenv, capture_output=True)
        time.sleep(1.0)
        w, h = size()
        for x, want in ((w // 4, "#ff0000"), (3 * w // 4, "#0000ff")):
            got = pick_at(x, h // 2)
            if got != want:
                failures.append(f"turned, scale 2, x={x} of {w}: {got!r}, not {want}")
        # 5. The client goes.
        p = start_pick()
        p.kill()
        p.wait()
        time.sleep(0.4)
        log.flush()
        if "wb-pick the client went" not in open(log_path).read():
            failures.append("a pick outlived its client")
        p = start_pick()
        subprocess.run(["wtype", "-k", "Escape"], env=cenv, capture_output=True)
        if "already" in answer(p):
            failures.append("another pick couldn't start after the client went")
    except (OSError, ValueError, KeyError, IndexError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        bg.kill()
        bg.wait()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if proc.returncode not in (0, None):
        failures.append(f"waybox exit status {proc.returncode}")
    if any(s in open(log_path).read() for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration color pick: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
