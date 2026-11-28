#!/usr/bin/env python3
"""Integration test: menu icons are loaded once, in the background, not
at every opening.

A root menu with three items, two icons (absolute paths to PNGs this test
writes), one of them used twice. Once Waybox is idle after starting, fonts
are warmed ("font-warmup": 1) and the menu's two icons preloaded on worker
threads ("wb-menu icons preloaded 2"); opening the menu twice then loads
none on the main thread (get_stalls' "menu-icon": 0). A Reconfigure clears
the cache (the theme may have changed) and preloads them again (2 more,
not 0), and an opening after it loads none either.

Skips (exit 77) without a headless display.

Usage: menu_icon_cache_test.py <waybox>
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
import wbtest

SKIP = 77


def png(path, rgb):
    """A 16x16 PNG of one color, written with zlib alone."""
    raw = b"".join(b"\x00" + bytes(rgb) * 16 for _ in range(16))

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    waybox = sys.argv[1]
    work = tempfile.mkdtemp(prefix="waybox-menuicons-", dir="/tmp")
    cfg = os.path.join(work, "config", "waybox")
    os.makedirs(cfg)
    red, blue = os.path.join(work, "red.png"), os.path.join(work, "blue.png")
    png(red, (220, 40, 40))
    png(blue, (40, 40, 220))
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write('<openbox_config xmlns="http://openbox.org/3.4/rc"><waybox><ipc enabled="yes"/>'
                '</waybox><menu><file>menu.xml</file><showIcons>yes</showIcons></menu>'
                '</openbox_config>\n')
    with open(os.path.join(cfg, "menu.xml"), "w") as f:
        f.write('<openbox_menu xmlns="http://openbox.org/3.4/menu"><menu id="root-menu" label="Root">'
                f'<item label="Red" icon="{red}"><action name="Execute"><command>true</command></action></item>'
                f'<item label="Blue" icon="{blue}"><action name="Execute"><command>true</command></action></item>'
                f'<item label="Red again" icon="{red}"><action name="Execute"><command>true</command></action></item>'
                '</menu></openbox_menu>\n')
    run = os.path.join(work, "run")
    os.makedirs(run, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", XDG_RUNTIME_DIR=run,
               XDG_CONFIG_HOME=os.path.join(work, "config"), WB_RC_XML="", WB_MENU_XML="")
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID"):
        env.pop(k, None)
    log = open(os.path.join(work, "waybox.log"), "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    failures = []
    try:
        sock = None
        for _ in range(100):
            time.sleep(0.05)
            s = [n for n in os.listdir(run) if n.startswith("wayland-") and not n.endswith(".lock")]
            if s:
                sock = s[0]
                break
        if sock is None:
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            return SKIP
        ipc_path = os.path.join(run, f"waybox-{sock}.sock")
        for _ in range(100):
            if os.path.exists(ipc_path):
                break
            time.sleep(0.05)

        def ipc(req):
            c = socket.socket(socket.AF_UNIX)
            c.connect(ipc_path)
            c.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                part = c.recv(65536)
                if not part:
                    break
                data += part
            c.close()
            return json.loads(data)

        def count(name):
            for s in ipc({"request": "get_stalls"})["scopes"]:
                if s["name"] == name:
                    return s["count"]
            return 0

        def preloaded():
            with open(os.path.join(work, "waybox.log")) as f:
                return [l.split("wb-menu icons preloaded ")[1].strip()
                        for l in f if "wb-menu icons preloaded " in l]
        for _ in range(50):
            if preloaded():
                break
            time.sleep(0.1)
        if preloaded() != ["2"]:
            failures.append(f"preloaded at startup: {preloaded()} (want ['2'])")
        if count("font-warmup") != 1:
            failures.append(f"fonts warmed {count('font-warmup')} times, not once")
        for _ in range(2):
            r = ipc({"request": "action", "action": "ShowMenu", "command": "root-menu"})
            if not r.get("ok"):
                failures.append(f"ShowMenu: {r}")
            time.sleep(0.2)
        if count("menu-icon") != 0:
            failures.append(f"two openings loaded {count('menu-icon')} icons themselves, not 0")
        ipc({"request": "reconfigure"})
        for _ in range(50):
            if len(preloaded()) >= 2:
                break
            time.sleep(0.1)
        if preloaded() != ["2", "2"]:
            failures.append(f"after Reconfigure: {preloaded()} (want ['2', '2']: cleared, loaded again)")
        ipc({"request": "action", "action": "ShowMenu", "command": "root-menu"})
        time.sleep(0.2)
        if count("menu-icon") != 0:
            failures.append(f"after Reconfigure, an opening loaded {count('menu-icon')} icons itself")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if failures:
        with open(os.path.join(work, "waybox.log")) as f:
            print(f.read()[-2000:], file=sys.stderr)
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration menu_icon_cache: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
