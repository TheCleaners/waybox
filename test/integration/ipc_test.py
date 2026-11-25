#!/usr/bin/env python3
"""Integration test for the scripting socket (<waybox><ipc enabled="yes"/>).

1. The socket is there, only for the user (0600).
2. A subscriber gets events: a window opening (new, focus), a desktop
   switch, a window closing, a reload.
3. waybox-msg: get_windows lists the window (focused), get_desktops the
   current one, actions run (GoToDesktop, Close by app_id), an unknown
   action is an error; programs Waybox starts get WAYBOX_SOCK.
4. Without the setting, there's no socket.

Skips (exit 77) without foot, or a headless display.

Usage: ipc_test.py <waybox> <waybox-msg>
"""
import json
import os
import queue
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, msg = sys.argv[1:]
    if shutil.which("foot") is None:
        print("SKIP: foot not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-ipc-", dir="/tmp")
    failures = []

    def session(tag, ipc, steps):
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>3</number></desktops>
  <waybox>{'<ipc enabled="yes"/>' if ipc else ''}</waybox>
</openbox_config>
""")
        runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
                   WB_MENU_XML="", ASAN_OPTIONS="detect_leaks=1:exitcode=99")
        for v in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK"):
            env.pop(v, None)
        lp = os.path.join(workdir, f"waybox-{tag}.log")
        log = open(lp, "w")
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
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            proc.kill()
            sys.exit(SKIP)
        cenv = dict(env, WAYLAND_DISPLAY=sock)
        children = []
        try:
            steps(cenv, runtime, sock, proc, children)
        except (RuntimeError, OSError, queue.Empty) as e:
            failures.append(f"{tag}: {e or 'timed out'}")
        finally:
            for c in children:
                if c.poll() is None:
                    c.kill()
            time.sleep(0.3)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"{tag}: waybox exit status {proc.returncode}")
            if any(s in open(lp).read() for s in ("Sanitizer", "Assertion", "runtime error")):
                failures.append(f"{tag}: sanitizer/assert output")

    def run_msg(cenv, *args):
        r = subprocess.run([msg, *args], env=cenv, capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def with_ipc(cenv, runtime, sock, proc, children):
        path = os.path.join(runtime, f"waybox-{sock}.sock")
        if not os.path.exists(path):
            raise RuntimeError("no scripting socket")
        if stat.S_IMODE(os.stat(path).st_mode) != 0o600:
            failures.append(f"the socket is mode {oct(os.stat(path).st_mode)}, not 0600")
        sub = socket.socket(socket.AF_UNIX)
        sub.connect(path)
        sub.sendall(b'{"request":"subscribe","events":["window","desktop","config"]}\n')
        events = queue.Queue()

        def reader():
            buf = b""
            while True:
                try:
                    data = sub.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    events.put(json.loads(line))
        threading.Thread(target=reader, daemon=True).start()
        if not events.get(timeout=5).get("ok"):
            raise RuntimeError("subscribe wasn't acknowledged")

        def expect(pred, what, timeout=5.0):
            end = time.time() + timeout
            while time.time() < end:
                try:
                    ev = events.get(timeout=max(0.01, end - time.time()))
                except queue.Empty:
                    break
                if pred(ev):
                    return ev
            failures.append(f"no event: {what}")
            return None

        cenv2 = dict(cenv, WAYBOX_SOCK=path)
        children.append(subprocess.Popen(["foot", "-a", "ipctest"], env=cenv,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        expect(lambda e: e.get("event") == "window" and e.get("change") == "new"
               and e["window"]["app_id"] == "ipctest", "window new")
        expect(lambda e: e.get("event") == "window" and e.get("change") == "focus"
               and e["window"]["app_id"] == "ipctest", "window focus")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_windows")
        wins = json.loads(out)["windows"] if rc == 0 else []
        if not any(w["app_id"] == "ipctest" and w["focused"] and w["id"] for w in wins):
            failures.append(f"get_windows didn't list the focused window: {out or err}")

        # A second window, focused: Close by app_id must pick the other one.
        children.append(subprocess.Popen(["foot", "-a", "second"], env=cenv,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        expect(lambda e: e.get("event") == "window" and e.get("change") == "focus"
               and e["window"]["app_id"] == "second", "second window focus")
        rc, out, err = run_msg(cenv2, "action", "Close", "window=app_id:ipctest")
        if rc != 0:
            failures.append(f"action Close failed: {err}")
        expect(lambda e: e.get("event") == "window" and e.get("change") == "close"
               and e["window"]["app_id"] == "ipctest", "window close")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_windows")
        left = [w["app_id"] for w in json.loads(out)["windows"]] if rc == 0 else []
        if left != ["second"]:
            failures.append(f"Close by app_id closed the wrong window: left {left}")

        rc, out, err = run_msg(cenv2, "action", "GoToDesktop", "to=2")
        if rc != 0:
            failures.append(f"action GoToDesktop failed: {err}")
        expect(lambda e: e.get("event") == "desktop" and e["desktop"]["number"] == 2,
               "desktop switch")
        rc, out, err = run_msg(cenv2, "get_desktops")
        current = [d["number"] for d in json.loads(out)["desktops"] if d["current"]] if rc == 0 else []
        if current != [2] or len(json.loads(out)["desktops"]) != 3:
            failures.append(f"get_desktops: {out or err}")


        rc, out, err = run_msg(cenv2, "action", "NoSuchAction")
        if rc == 0 or "unknown action" not in err:
            failures.append(f"an unknown action wasn't an error: {rc} {err}")

        marker = os.path.join(runtime, "sock-env")
        run_msg(cenv2, "action", "Execute", f"command=sh -c 'echo $WAYBOX_SOCK > {marker}'")
        time.sleep(0.8)
        if not os.path.exists(marker) or open(marker).read().strip() != path:
            failures.append("a program started by Waybox didn't get WAYBOX_SOCK")

        proc.send_signal(signal.SIGUSR2)
        expect(lambda e: e.get("event") == "config", "config reload")
        sub.close()
    session("on", True, with_ipc)

    def without(cenv, runtime, sock, proc, children):
        if os.path.exists(os.path.join(runtime, f"waybox-{sock}.sock")):
            failures.append("a socket without <ipc enabled=\"yes\"/>")
        rc, out, err = run_msg(cenv, "get_windows")
        if rc == 0:
            failures.append("waybox-msg succeeded without a socket")
    session("off", False, without)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration ipc: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
