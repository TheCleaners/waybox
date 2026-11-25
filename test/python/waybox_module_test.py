#!/usr/bin/env python3
"""Unit test of the waybox Python module (python/waybox), against a fake
socket: requests and answers, errors, finding the socket, timeouts, long
lines, events.

Usage: waybox_module_test.py <python-source-dir>
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, sys.argv[1])
import waybox  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


class FakeWaybox:
    """Answers each request line with what `answer(request)` returns (a
    dict, or raw bytes); records the requests."""

    def __init__(self, answer):
        self.dir = tempfile.mkdtemp(prefix="wb-py-test-")
        self.path = os.path.join(self.dir, "waybox-test.sock")
        self.answer = answer
        self.requests = []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.path)
        self.srv.listen(4)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.client, args=(conn,), daemon=True).start()

    def client(self, conn):
        buf = b""
        with conn:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    req = json.loads(line)
                    self.requests.append(req)
                    out = self.answer(req)
                    if out is None:
                        continue
                    for piece in (out if isinstance(out, list) else [out]):
                        conn.sendall(piece if isinstance(piece, bytes)
                                     else (json.dumps(piece) + "\n").encode())

    def close(self):
        self.srv.close()


def answers(req):
    r = req["request"]
    if r == "get_version":
        return {"ok": True, "name": "waybox", "version": "1.2.3"}
    if r == "get_windows":
        return {"ok": True, "windows": [{"id": "a", "app_id": "foot", "focused": False},
                                        {"id": "b", "app_id": "kgx", "focused": True}]}
    if r == "identify":
        if req.get("output") == "NOPE":
            return {"ok": False, "error": "no such monitor"}
        return {"ok": True, "shown": 1 if req.get("output") else 2}
    if r == "get_desktops":
        return {"ok": True, "desktops": [{"number": 1, "current": True}]}
    if r == "action":
        if req.get("window") == "nope":
            return {"ok": False, "error": "no such window: nope"}
        return {"ok": True}
    if r == "subscribe":
        return [{"ok": True},
                {"event": "window", "change": "new", "window": {"id": "c"}},
                {"event": "desktop", "current": 2}]
    if r == "silent":
        return None
    if r == "notjson":
        return b"garbage\n"
    return {"ok": False, "error": f"unknown request: {r}"}


fake = FakeWaybox(answers)
try:
    with waybox.connect(fake.path) as wb:
        check(wb.version() == "1.2.3", "version")
        check(wb.identify() == 2, "identify")
        check(wb.identify("DP-1", ms=500) == 1, "identify one")
        check(fake.requests[-1] == {"request": "identify", "output": "DP-1", "ms": 500},
              f"identify request: {fake.requests[-1]}")
        try:
            wb.identify("NOPE")
            check(False, "identify NOPE raised")
        except waybox.RequestFailed as e:
            check("no such monitor" in str(e), "identify NOPE error")
        ws = wb.windows()
        check([w["id"] for w in ws] == ["a", "b"], f"windows: {ws}")
        check(wb.focused()["id"] == "b", "focused")
        check(wb.desktops()[0]["current"] is True, "desktops")
        # Several requests on one connection, in order.
        wb.go_to_desktop(2)
        check(fake.requests[-1] == {"request": "action", "action": "GoToDesktop", "to": "2"},
              f"go_to_desktop sent {fake.requests[-1]}")
        wb.send_to_desktop("next", window="app_id:foot", follow=True)
        check(fake.requests[-1] == {"request": "action", "action": "SendToDesktop",
                                    "to": "next", "window": "app_id:foot", "follow": True},
              f"send_to_desktop sent {fake.requests[-1]}")
        wb.execute("foot -e htop")
        check(fake.requests[-1].get("command") == "foot -e htop", "execute")
        wb.focus("a")
        check(fake.requests[-1] == {"request": "action", "action": "Focus", "window": "a"},
              "focus")
        # Unset options aren't sent.
        wb.action("Close")
        check(fake.requests[-1] == {"request": "action", "action": "Close"}, "no None fields")
        # Waybox's errors.
        try:
            wb.close_window("nope")
            check(False, "an error answer didn't raise")
        except waybox.RequestFailed as e:
            check(str(e) == "no such window: nope", f"error message: {e}")
        try:
            wb.request("frobnicate")
            check(False, "an unknown request didn't raise")
        except waybox.RequestFailed:
            pass
    # Not JSON, and no answer in time.
    with waybox.connect(fake.path, timeout=0.5) as wb:
        try:
            wb.request("notjson")
            check(False, "garbage didn't raise")
        except waybox.WayboxError as e:
            check(not isinstance(e, waybox.RequestFailed), "garbage as RequestFailed")
    with waybox.connect(fake.path, timeout=0.3) as wb:
        t = time.time()
        try:
            wb.request("silent")
            check(False, "no answer didn't raise")
        except waybox.WayboxError as e:
            check("in time" in str(e), f"timeout message: {e}")
        check(time.time() - t < 2, "the timeout wasn't kept")
    # Events.
    events = []
    for e in waybox.subscribe(["window", "desktop"], path=fake.path):
        events.append(e)
        if len(events) == 2:
            break
    check(events[0]["change"] == "new" and events[1]["event"] == "desktop", f"events: {events}")
    check(fake.requests[-1] == {"request": "subscribe", "events": ["window", "desktop"]},
          "subscribe request")
    try:
        next(waybox.connect(fake.path).subscribe(["nonsense"]))
        check(False, "an unknown event didn't raise")
    except ValueError:
        pass
finally:
    fake.close()

# Finding the socket.
env = dict(os.environ)
try:
    os.environ["WAYBOX_SOCK"] = "/x/y.sock"
    check(waybox.socket_path() == "/x/y.sock", "WAYBOX_SOCK")
    del os.environ["WAYBOX_SOCK"]
    os.environ["XDG_RUNTIME_DIR"] = "/run/user/7"
    os.environ["WAYLAND_DISPLAY"] = "wayland-3"
    check(waybox.socket_path() == "/run/user/7/waybox-wayland-3.sock", "from the display")
    check(waybox.socket_path("wayland-9") == "/run/user/7/waybox-wayland-9.sock", "display arg")
    os.environ["WAYLAND_DISPLAY"] = "/tmp/run/wayland-5"
    check(waybox.socket_path() == "/run/user/7/waybox-wayland-5.sock", "absolute display")
    del os.environ["WAYLAND_DISPLAY"]
    try:
        waybox.socket_path()
        check(False, "no display didn't raise")
    except waybox.NotRunning:
        pass
    try:
        waybox.connect("/nonexistent/waybox.sock")
        check(False, "no socket didn't raise")
    except waybox.NotRunning as e:
        check("ipc enabled" in str(e), f"NotRunning says how to fix it: {e}")
finally:
    os.environ.clear()
    os.environ.update(env)

for f in failures:
    print(f"FAIL: {f}", file=sys.stderr)
if failures:
    sys.exit(1)
print("waybox python module: OK")
