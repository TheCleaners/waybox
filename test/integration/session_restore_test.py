#!/usr/bin/env python3
"""Integration test: session restore (xdg-session-management-v1).

1. A new session: the app gets an id; its window, snapped to the right half
   and sent to desktop 2, is saved (sessions.json) a moment later.
2. Recovering (reason "recover", same id): the session is restored, the
   window told so before its first configure, which has its old size; it
   opens where it was, on desktop 2.
3. Another client taking the same id replaces the first ("replaced");
   launched (reason "launch") it gets size and place back, but opens on
   the desktop shown, not its old one.
4. A name the session doesn't know: placed as a new window. Asking to
   restore after the first commit is a protocol error.
5. Waybox restarted: the session is still there and restores.
6. "remove": the session is gone from sessions.json.
7. <sessionRestore enabled="no"/>: a known id isn't restored (a new one is
   given), nothing is placed from it.
8. A save that fails (the state directory unwritable) isn't forgotten: it's
   tried again, and lands once the directory can be written.
9. A file of a newer format isn't read, and isn't written over.
Skips (77) without a headless display.

Usage: session_restore_test.py <waybox> <session_client>
"""
import json
import os
import queue
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import wbtest

SKIP = 77

RC = """<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>2</number></desktops>
  <waybox><ipc enabled="yes"/>{extra}</waybox>
</openbox_config>
"""


class Client:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, text=True, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)
        wbtest.reap_on_exit(self.proc)
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())
        self.lines.put(None)

    def wait_for(self, prefix, timeout=5.0):
        end = time.time() + timeout
        for line in self.seen:
            if line is not None and line.startswith(prefix):
                return line
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.seen.append(line)
            if line is None:
                break
            if line.startswith(prefix):
                return line
        return None

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        return self.wait_for("ok")

    def quit(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def ipc(runtime, display, request):
    path = os.path.join(runtime, f"waybox-{display}.sock")
    for _ in range(20):  # a moment for it to be there; then say which
        if os.path.exists(path):
            break
        time.sleep(0.1)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
        c.settimeout(5)
        try:
            c.connect(path)
        except FileNotFoundError as e:
            raise RuntimeError(f"no scripting socket {path} "
                               f"(there: {sorted(os.listdir(runtime))})") from e
        c.sendall((json.dumps(request) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = c.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)


class Waybox:
    def __init__(self, waybox, workdir, tag, extra=""):
        cfg = os.path.join(workdir, f"config-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(RC.format(extra=extra))
        self.runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(self.runtime, 0o700)
        self.env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                        XDG_RUNTIME_DIR=self.runtime, WB_RC_XML="", WB_MENU_XML="",
                        XDG_CONFIG_HOME=cfg, XDG_STATE_HOME=os.path.join(workdir, "state"),
                        ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux")
                                      else "") + "exitcode=99")
        self.env.pop("WAYLAND_DISPLAY", None)
        self.env.pop("DISPLAY", None)
        self.log_path = os.path.join(workdir, f"waybox-{tag}.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([waybox, "--debug"], env=self.env, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        wbtest.reap_on_exit(self.proc)
        self.sock = None
        for _ in range(50):
            time.sleep(0.1)
            if self.proc.poll() is not None:
                break
            socks = [n for n in os.listdir(self.runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                self.sock = socks[0]
                break
        self.cenv = dict(self.env, WAYLAND_DISPLAY=self.sock or "")

    def windows(self):
        return {w["app_id"]: w for w in
                ipc(self.runtime, self.sock, {"request": "get_windows"})["windows"]}

    def action(self, name, window, **kw):
        ipc(self.runtime, self.sock, dict(request="action", action=name, window=window, **kw))

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()
        problems = []
        if self.proc.returncode not in (0, None):
            problems.append(f"waybox exit status {self.proc.returncode}")
        text = open(self.log_path).read()
        if any(s in text for s in ("Sanitizer", "Assertion", "runtime error")):
            problems.append("sanitizer/assert output")
        return problems


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-session-", dir="/tmp")
    store = os.path.join(workdir, "state", "waybox", "sessions.json")
    failures = []

    def client(wb, app_id, sid, reason, mode, name):
        return Client([client_bin, app_id, sid, reason, mode, name], wb.cenv)

    def first_size(c):
        line = c.wait_for("configured ")
        return tuple(int(v) for v in line.split()[1:]) if line else None

    def saved(sid):
        try:
            return json.load(open(store))["sessions"].get(sid)
        except (OSError, ValueError):
            return None

    wb = Waybox(waybox, workdir, "one")
    if wb.sock is None:
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    clients = []
    sid = None
    expected = None
    try:
        out = ipc(wb.runtime, wb.sock, {"request": "get_outputs"})["outputs"][0]
        ow, oh = out["width"], out["height"]

        # 1. A new session; the window moved, resized and sent away.
        a = client(wb, "sess-a", "-", "launch", "add", "main")
        clients.append(a)
        created = a.wait_for("created ")
        if created is None or a.wait_for("configured ") is None:
            raise RuntimeError(f"no session or no window: {a.seen}")
        sid = created.split()[1]
        time.sleep(0.4)
        wid = wb.windows()["sess-a"]["id"]
        wb.action("SnapToEdge", wid, direction="right")
        time.sleep(1.6)  # saved a second after the first change
        # A change on its own, after that save: saved by itself, with the
        # app still running.
        wb.action("SnapToEdge", wid, direction="left")
        time.sleep(1.6)
        s = saved(sid)
        m = s["windows"]["main"] if s and "main" in s.get("windows", {}) else None
        if m is None or m["x"] >= ow // 3:
            failures.append(f"a later change wasn't saved: {m}")
        wb.action("SnapToEdge", wid, direction="right")
        time.sleep(0.4)
        wb.action("SendToDesktop", wid, to="2", follow=False)
        time.sleep(0.4)
        w = wb.windows()["sess-a"]
        expected = (w["x"], w["y"], w["width"], w["height"])
        if expected[0] < ow // 3 or expected[2] > ow * 2 // 3:
            failures.append(f"the window wasn't snapped right: {w}")
        time.sleep(1.5)
        s = saved(sid)
        if s is None or "main" not in s.get("windows", {}):
            failures.append(f"the session wasn't saved: {s}")
        else:
            m = s["windows"]["main"]
            if (m["x"], m["y"], m["width"], m["height"]) != expected or m["desktop"] != 1:
                failures.append(f"saved wrongly: {m}, expected {expected} on desktop 2")
        a.quit()
        time.sleep(0.3)

        # 2. Recovering: back as it was, desktop included.
        b = client(wb, "sess-b", sid, "recover", "restore", "main")
        clients.append(b)
        if b.wait_for("session-restored") is None:
            failures.append(f"the session wasn't restored: {b.seen}")
        if b.wait_for("toplevel-restored") is None:
            failures.append(f"the window wasn't told it's restored: {b.seen}")
        size = first_size(b)
        if size != expected[2:]:
            failures.append(f"the first configure wasn't its old size: {size} != {expected[2:]}")
        time.sleep(0.4)
        w = wb.windows().get("sess-b")
        if w is None or (w["x"], w["y"], w["width"], w["height"]) != expected:
            failures.append(f"recovered window misplaced: {w}, expected {expected}")
        elif w["desktop"] != 2:
            failures.append(f"recovered window not on its desktop: {w['desktop']}")

        # 3. Taken over by another client, launched: place yes, desktop no.
        c = client(wb, "sess-c", sid, "launch", "restore", "main")
        clients.append(c)
        if b.wait_for("replaced") is None:
            failures.append("the first client wasn't told it was replaced")
        if c.wait_for("toplevel-restored") is None:
            failures.append(f"the new client's window wasn't restored: {c.seen}")
        c.wait_for("configured ")
        time.sleep(0.4)
        w = wb.windows().get("sess-c")
        if w is None or (w["x"], w["y"], w["width"], w["height"]) != expected:
            failures.append(f"launched window misplaced: {w}")
        elif w["desktop"] != 1:
            failures.append(f"a launched app got its old desktop: {w['desktop']}")
        b.quit()
        c.quit()

        # 4. An unknown name; restoring too late.
        d = client(wb, "sess-d", sid, "recover", "restore", "other")
        clients.append(d)
        size = first_size(d)
        if d.wait_for("toplevel-restored", timeout=0.5) is not None:
            failures.append("a name the session doesn't know was 'restored'")
        if size != (300, 200):
            failures.append(f"an unknown window wasn't left its own size: {size}")
        d.quit()
        e = client(wb, "sess-e", sid, "recover", "late", "main")
        clients.append(e)
        if e.wait_for("error") is None:
            failures.append(f"restoring after the first commit wasn't an error: {e.seen}")
        failures += [f"one: {p}" for p in wb.stop()]

        # 5. After a restart.
        wb = Waybox(waybox, workdir, "two")
        f = client(wb, "sess-f", sid, "recover", "restore", "main")
        clients.append(f)
        if f.wait_for("toplevel-restored") is None:
            failures.append(f"not restored after a restart: {f.seen}")
        f.wait_for("configured ")
        time.sleep(0.4)
        w = wb.windows().get("sess-f")
        if w is None or (w["x"], w["y"], w["width"], w["height"]) != expected:
            failures.append(f"misplaced after a restart: {w}")
        # 6. Removed.
        f.send("remove")
        time.sleep(1.5)
        if saved(sid) is not None:
            failures.append("a removed session is still saved")
        f.quit()
        failures += [f"two: {p}" for p in wb.stop()]

        # 7. Turned off: nothing restored, and a known id isn't honoured.
        wb = Waybox(waybox, workdir, "three")
        g = client(wb, "sess-g", "-", "launch", "add", "main")
        clients.append(g)
        sid2 = g.wait_for("created ").split()[1]
        g.wait_for("configured ")
        wb.action("SnapToEdge", wb.windows()["sess-g"]["id"], direction="right")
        time.sleep(1.6)
        g.quit()
        failures += [f"three: {p}" for p in wb.stop()]
        wb = Waybox(waybox, workdir, "off", extra='<sessionRestore enabled="no"/>')
        h = client(wb, "sess-h", sid2, "recover", "restore", "main")
        clients.append(h)
        created = h.wait_for("created ")
        if created is None or created.split()[1] == sid2:
            failures.append(f"with it off, the known session was honoured: {h.seen}")
        if h.wait_for("toplevel-restored", timeout=1.0) is not None:
            failures.append("with it off, a window was restored")
        h.quit()
        failures += [f"off: {p}" for p in wb.stop()]

        # 8. The directory can't be written: the save fails, is kept, and
        # is tried again (5 s on) until it can.
        state_dir = os.path.dirname(store)
        wb = Waybox(waybox, workdir, "retry")
        os.chmod(state_dir, 0o500)
        try:
            r = client(wb, "sess-r", "-", "launch", "add", "main")
            clients.append(r)
            sid3 = r.wait_for("created ").split()[1]
            r.wait_for("configured ")
            wb.action("SnapToEdge", wb.windows()["sess-r"]["id"], direction="right")
            time.sleep(1.6)
            if saved(sid3) is not None:
                failures.append("saved into a directory that can't be written?")
            if "sessions: can't write" not in open(wb.log_path).read():
                failures.append("a failed save wasn't logged")
        finally:
            os.chmod(state_dir, 0o700)
        for _ in range(80):
            time.sleep(0.1)
            if saved(sid3) is not None:
                break
        if saved(sid3) is None:
            failures.append("a failed save wasn't tried again once it could be")
        r.quit()
        failures += [f"retry: {p}" for p in wb.stop()]

        # 9. A newer format's file: left as it is, byte for byte.
        newer = '{"version":99,"sessions":{"x":{"future":true}}}\n'
        with open(store, "w") as f:
            f.write(newer)
        wb = Waybox(waybox, workdir, "newer")
        n = client(wb, "sess-n", "-", "launch", "add", "main")
        clients.append(n)
        n.wait_for("created ")
        n.wait_for("configured ")
        wb.action("SnapToEdge", wb.windows()["sess-n"]["id"], direction="right")
        time.sleep(1.6)
        n.quit()
        failures += [f"newer: {p}" for p in wb.stop()]
        if open(store).read() != newer:
            failures.append("a newer format's sessions.json was written over")
        if "from a newer Waybox" not in open(wb.log_path).read():
            failures.append("a newer format's file wasn't said so")
    except (RuntimeError, OSError, KeyError, IndexError, AttributeError) as e:
        failures.append(f"{type(e).__name__}: {e}")
        failures += wb.stop() if wb.proc.poll() is None else []
    finally:
        for c in clients:
            if c.proc.poll() is None:
                c.proc.kill()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration session restore: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
