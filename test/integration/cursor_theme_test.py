#!/usr/bin/env python3
"""Integration test: the cursor theme and size (rc.xml <cursor>).

A made-up cursor theme, written here (Xcursor files: a solid red cursor at
16 px and a solid blue one at 48 px), found through XCURSOR_PATH. The
pointer over the empty desktop, screenshots with the cursor in them:
1. Started with XCURSOR_THEME and XCURSOR_SIZE=16, no <cursor>: small
   and red (the middle of a 48 px square isn't covered).
2. <cursor size="48"/>, Reconfigure: big and blue, without a restart; a
   program started after it gets XCURSOR_SIZE=48 and the theme.
3. <cursor> gone, Reconfigure: back to 16 (as Waybox was started).
The pointer isn't moved after a Reconfigure: the cursor changes where it
is. A scale change after it loads the cursor again (under ASan: nothing
of the old theme is used after it's gone).
Skips (77) without grim or wlrctl, or a headless display.

Usage: cursor_theme_test.py <waybox>
"""
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


def xcursor(images):
    """An Xcursor file: images = [(size, (a, r, g, b))], each a solid
    square, hotspot at its top left."""
    header = struct.pack("<4sIII", b"Xcur", 16, 0x10000, len(images))
    toc, chunks = b"", b""
    pos = 16 + 12 * len(images)
    for size, (a, r, g, b) in images:
        pixel = struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
        chunk = struct.pack("<IIIIIIIII", 36, 0xfffd0002, size, 1, size, size, 0, 0, 50)
        chunk += pixel * (size * size)
        toc += struct.pack("<III", 0xfffd0002, size, pos)
        chunks += chunk
        pos += len(chunk)
    return header + toc + chunks


def rc(cursor, marker):
    startup = (f'<startup><program name="env-check" restart="no">'
               f'echo "$XCURSOR_THEME $XCURSOR_SIZE" &gt; {marker}</program></startup>'
               if marker else "")
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{cursor}<virtualInput allow="always"/>{startup}</waybox>
</openbox_config>
"""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("grim", "wlrctl"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-cursor-", dir="/tmp")
    icons = os.path.join(workdir, "icons")
    cursors = os.path.join(icons, "TestCursors", "cursors")
    os.makedirs(cursors)
    data = xcursor([(16, (255, 255, 0, 0)), (48, (255, 0, 0, 255))])
    for name in ("default", "left_ptr"):
        with open(os.path.join(cursors, name), "wb") as f:
            f.write(data)
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    marker = os.path.join(workdir, "env")
    with open(rc_path, "w") as f:
        f.write(rc("", None))
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=cfg, WB_RC_XML="", WB_MENU_XML="",
               XCURSOR_PATH=icons, XCURSOR_THEME="TestCursors", XCURSOR_SIZE="16",
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
    at = (400, 300)

    def point():
        subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                       capture_output=True)
        subprocess.run(["wlrctl", "pointer", "move", str(at[0]), str(at[1])], env=cenv,
                       capture_output=True)
        time.sleep(0.3)

    def pixel(dx, dy):
        r = subprocess.run(["grim", "-c", "-t", "ppm", "-g", f"{at[0] + dx},{at[1] + dy} 1x1", "-"],
                           env=cenv, capture_output=True, timeout=10)
        return r.stdout[-3:]

    def reload(text):
        # The pointer isn't moved: the cursor must change where it is.
        with open(rc_path, "w") as f:
            f.write(text)
        proc.send_signal(signal.SIGUSR2)
        time.sleep(0.8)

    red, blue = b"\xff\x00\x00", b"\x00\x00\xff"
    try:
        point()
        if pixel(4, 4) != red or pixel(30, 30) in (red, blue):
            failures.append(f"started at 16: {pixel(4, 4)} {pixel(30, 30)}")
        reload(rc('<cursor theme="TestCursors" size="48"/>', marker))
        if pixel(4, 4) != blue or pixel(30, 30) != blue:
            failures.append(f"<cursor size=48> after Reconfigure: {pixel(4, 4)} {pixel(30, 30)}")
        # A new scale makes the cursor load again from the theme in use:
        # the old one must not be (ASan: no use after free).
        if shutil.which("wlr-randr"):
            subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--scale", "2"], env=cenv,
                           capture_output=True)
            time.sleep(0.5)
            subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--scale", "1"], env=cenv,
                           capture_output=True)
            time.sleep(0.5)
        time.sleep(0.5)
        got = open(marker).read().strip() if os.path.exists(marker) else ""
        if got != "TestCursors 48":
            failures.append(f"a program started after it got {got!r}")
        reload(rc("", None))
        if pixel(4, 4) != red or pixel(30, 30) in (red, blue):
            failures.append(f"<cursor> gone: not back to 16: {pixel(4, 4)} {pixel(30, 30)}")
        log.flush()
        if "wb-cursor theme TestCursors size 48" not in open(log_path).read():
            failures.append("the change wasn't logged")
    except OSError as e:
        failures.append(str(e))
    finally:
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
    print("integration cursor theme: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
