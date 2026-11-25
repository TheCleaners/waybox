#!/usr/bin/env python3
"""Integration test: wbconf's wallpaper page (Page=wallpaper).

Two made-up wallpaper programs, in a home with a folder of wallpapers:
- fakebg (an image): a thumbnail of the folder is chosen (the folder is
  the current image's, found by itself), and the mode from the page:
  the command has the new image and the mode;
- fakerand (an image or a folder): "a random one from the folder", and
  "change every 15 minutes": the command has the folder and -e 15.
Saved with Ctrl+S; rc.xml checked. A thumbnail that isn't in the grid
can't be chosen: a file that isn't an image isn't offered.
Skips (exit 77) without wtype, or a headless display.

Usage: wbconf_wallpaper_test.py <waybox> <wbconf> <schema-dir>
"""
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
import wbtest

SKIP = 77

INTEGRATION = """[Integration]
Name={name}
Category=Wallpaper
Comment=A made-up wallpaper program
Exec={name}
Page=wallpaper

[Option image]
Label=Image
Flag=-i
Type={image_type}

[Option every]
Label=Every
Flag=-e
Type=number
Min=1

[Option colour]
Label=Colour
Flag=-c
Type=color

[Option mode]
Label=Mode
Flag=-m
Type=choice
Choices=fill,fit,stretch,center,tile
Default=fill
"""

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>
    <startup>
      <program name="fakebg" enabled="no">fakebg -i ~/art/walls/a.png</program>
      <program name="fakerand" enabled="no">fakerand -i ~/art/walls/a.png</program>
    </startup>
  </waybox>
</openbox_config>
"""


def png(path, w, h, rgb):
    """A plain PNG of one colour (no image tools needed)."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    if shutil.which("wtype") is None:
        print("SKIP: wtype not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-wbwall-", dir="/tmp")
    home = os.path.join(workdir, "home")
    walls = os.path.join(home, "art", "walls")
    os.makedirs(walls)
    png(os.path.join(walls, "a.png"), 64, 40, (20, 60, 120))
    png(os.path.join(walls, "b.png"), 64, 40, (200, 120, 40))
    with open(os.path.join(walls, "notes.txt"), "w") as f:
        f.write("not an image\n")
    bindir = os.path.join(workdir, "bin")
    os.makedirs(bindir)
    integ = os.path.join(workdir, "share", "waybox", "integrations")
    os.makedirs(integ)
    for name, image_type in (("fakebg", "image"), ("fakerand", "image-or-folder")):
        with open(os.path.join(bindir, name), "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(os.path.join(bindir, name), 0o755)
        with open(os.path.join(integ, name + ".integration"), "w") as f:
            f.write(INTEGRATION.format(name=name, image_type=image_type))
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               XDG_DATA_HOME=os.path.join(home, ".local", "share"),
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", NO_AT_BRIDGE="1",
                WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                WBCONF_TEST_SET="wallpaper.fakebg=~/art/walls/b.png;"
                                # Not offered: choosing it does nothing.
                                "wallpaper.fakebg=~/art/walls/notes.txt;"
                                "wallpaper.fakebg.mode=fit;"
                                "wallpaper.fakerand.random=on;"
                                "wallpaper.fakerand.every=15",
                XDG_DATA_DIRS=os.path.join(workdir, "share"),
                PATH=bindir + ":" + os.environ.get("PATH", ""))
    failures = []
    app = None
    try:
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        wbtest.reap_on_exit(app)
        time.sleep(4)
        if app.poll() is not None:
            raise RuntimeError(f"wbconf exited ({app.returncode})")
        subprocess.run(["wtype", "-M", "ctrl", "s", "-m", "ctrl"], env=cenv,
                       capture_output=True, timeout=10)
        time.sleep(1.5)
        out = open(rc_path).read()

        def command(name):
            m = re.search(rf'<program name="{name}"[^>]*>([^<]*)</program>', out)
            return m.group(1) if m else None
        want = {
            "fakebg": "fakebg -i ~/art/walls/b.png -m fit",
            "fakerand": "fakerand -i ~/art/walls -e 15",
        }
        for name, w in want.items():
            if command(name) != w:
                failures.append(f"{name}'s command: {command(name)!r}, not {w!r}")
    except (RuntimeError, OSError) as e:
        failures.append(str(e))
    finally:
        if app is not None and app.poll() is None:
            app.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(open(rc_path).read(), file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf wallpaper: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
