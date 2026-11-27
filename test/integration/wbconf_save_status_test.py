#!/usr/bin/env python3
"""Integration test: what wbconf's Save says about Waybox reloading.

Waybox (headless) gives wbconf a display; a stand-in scripting socket
(WAYBOX_SOCK) answers its "reconfigure" request as each case needs:

1. Applied: "Saved and applied."
2. Refused (an rc.xml Waybox can't read): "Saved, but Waybox kept its
   current settings: <why>".
3. A later file fails (the GTK settings: settings.ini is a folder): Waybox is
   still asked, and the status says both: "Saved rc.xml and applied it,
   but not the GTK settings: ...".
4. No socket: the signal, worded for what it can confirm ("asked Waybox
   to reload"), and Waybox reconfigures.

Skips (exit 77) without a headless display.

Usage: wbconf_save_status_test.py <waybox> <wbconf> <schema-dir>
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77

RC = ('<?xml version="1.0" encoding="UTF-8"?>\n'
      '<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')


class StandIn:
    """A scripting socket answering "reconfigure" with `answer`."""

    def __init__(self, path):
        self.path = path
        self.answer = {"ok": True, "applied": True}
        self.asked = []
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(16)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            with c:
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = c.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                try:
                    req = json.loads(buf).get("request", "")
                except ValueError:
                    req = ""
                self.asked.append(req)
                reply = self.answer if req == "reconfigure" else {"ok": True}
                c.sendall((json.dumps(reply) + "\n").encode())


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-wbsavestatus-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    with open(rc_path, "w") as f:
        f.write(RC)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=0")
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
    stand_in = StandIn(os.path.join(workdir, "stand-in.sock"))
    failures = []

    def save(number, sock_path, gtk_ini_is_folder=False):
        """wbconf changes desktops.number (and a GTK setting), saves; its status."""
        with open(rc_path, "w") as f:
            f.write(RC)
        gtk = os.path.join(cfg, "gtk-4.0")
        shutil.rmtree(gtk, ignore_errors=True)
        if os.path.isfile(gtk):
            os.unlink(gtk)
        if gtk_ini_is_folder:
            # Its settings.ini a folder: there, and not writable as a file.
            os.makedirs(os.path.join(gtk, "settings.ini"))
        cenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland",
                    DBUS_SESSION_BUS_ADDRESS="disabled:",  # never the user's wbconf
                    GSETTINGS_BACKEND="memory",            # never the user's settings
                    WBCONF_SCHEMA_DIR=schema, WAYBOX_PID=str(proc.pid),
                    WAYBOX_SOCK=sock_path,
                    WBCONF_TEST_SET=f"desktops.number={number};gtk.scheme=prefer-dark",
                    WBCONF_TEST_TOC="save;status")
        app = subprocess.Popen([wbconf], env=cenv, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
        wbtest.reap_on_exit(app)
        time.sleep(5)
        app.kill()
        _, err = app.communicate()
        dumps = [l[len("status-dump:"):] for l in err.splitlines()
                 if l.startswith("status-dump:")]
        return dumps[-1] if dumps else None

    try:
        # 1. Applied.
        stand_in.answer = {"ok": True, "applied": True}
        status = save(5, stand_in.path)
        if status != "Saved and applied.":
            failures.append(f"applied: {status!r}")
        if "reconfigure" not in stand_in.asked:
            failures.append(f"wbconf didn't ask Waybox to reconfigure: {stand_in.asked}")
        # 2. Refused.
        stand_in.answer = {"ok": False, "applied": False, "error": "rc.xml, line 3: oops"}
        status = save(6, stand_in.path)
        if status != "Saved, but Waybox kept its current settings: rc.xml, line 3: oops":
            failures.append(f"refused: {status!r}")
        # 3. A later file fails: still asked, both said.
        stand_in.answer = {"ok": True, "applied": True}
        stand_in.asked.clear()
        status = save(7, stand_in.path, gtk_ini_is_folder=True)
        if status is None or not status.startswith(
                "Saved rc.xml and applied it, but not the GTK settings: "):
            failures.append(f"partial: {status!r}")
        if "reconfigure" not in stand_in.asked:
            failures.append("a later file failing kept Waybox from being asked")
        if "<number>7</number>" not in open(rc_path).read():
            failures.append("rc.xml wasn't written before the later file failed")
        # 4. No socket: the signal.
        before = open(log_path).read().count("Reconfigured")
        status = save(8, os.path.join(workdir, "no-such.sock"))
        if status != "Saved; asked Waybox to reload.":
            failures.append(f"signalled: {status!r}")
        time.sleep(0.5)
        log.flush()
        if open(log_path).read().count("Reconfigured") <= before:
            failures.append("the signal didn't reach Waybox")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf_save_status: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
