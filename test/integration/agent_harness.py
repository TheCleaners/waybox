"""A stand-in compositor for testing the hub on its own (no Waybox): it
starts the real waybox-agent-hub with both sockets, as a compositor does
(fd 3: org.freedesktop.AgentCompositor1, peer-to-peer D-Bus; fd 4: its
tools, MCP), and records what the hub does.

Its tools (manifest org.test.Harness, short name "harness"):
- sleep(ms, progress_ms?): answers after `ms`, reporting progress every
  `progress_ms` if given; counts how many run at once.
- echo(text?, password?): answers at once (password is a Secret).
- zap: a destroy tool ("safe" never offers it).
The hub's cancellations are recorded (`cancelled`), and `drop_tools()`
closes the tools socket mid-call (a provider going away).
"""
import json
import os
import socket
import subprocess
import threading
import time

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

PATH = "/org/freedesktop/AgentCompositor1"
IFACE = "org.freedesktop.AgentCompositor1"

MANIFEST = """[Provider]
Version=1
Id=org.test.Harness
Name=Harness
Mode=compositor
[Tool sleep]
Sensitivity=read
Untrusted=no
Description=Sleeps.
[Tool echo]
Sensitivity=read
Untrusted=no
Secret=password
Description=Echoes.
[Tool zap]
Sensitivity=destroy
Description=Zaps.
"""


class Harness:
    def __init__(self, hub, work, xml_path, env_extra=None):
        self.work = work
        self.locked = False
        self.calls = []          # (name, arguments) as received
        self.cancelled = []      # request ids the hub cancelled
        self.running = 0
        self.max_running = 0
        self.lock = threading.Lock()
        data = os.path.join(work, "data", "agents", "tools")
        os.makedirs(data, exist_ok=True)
        with open(os.path.join(data, "org.test.Harness.ini"), "w") as f:
            f.write(MANIFEST)
        self.run_dir = os.path.join(work, "run")
        os.makedirs(self.run_dir, 0o700, exist_ok=True)
        self.state_dir = os.path.join(work, "state")
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.run_dir,
                        XDG_CONFIG_HOME=os.path.join(work, "config"),
                        XDG_CONFIG_DIRS=os.path.join(work, "etc"),
                        XDG_DATA_HOME=os.path.join(work, "data"),
                        XDG_DATA_DIRS=os.path.join(work, "data-dirs"),
                        XDG_STATE_HOME=self.state_dir, **(env_extra or {}))
        for k in ("AGENT_COMPOSITOR_FD", "AGENT_COMPOSITOR_TOOLS_FD", "AGENT_TOOLS_SOCKET"):
            self.env.pop(k, None)
        with open(xml_path) as f:
            self.node = Gio.DBusNodeInfo.new_for_xml(f.read())
        self.ctx = GLib.MainContext.new()
        self.loop = GLib.MainLoop.new(self.ctx, False)
        self.conn = None
        dbus_ours, dbus_theirs = socket.socketpair()
        self.tools_ours, tools_theirs = socket.socketpair()
        self.log = open(os.path.join(work, "hub.log"), "w")
        # (the shell puts them at 3 and 4, as a compositor does)
        self.hub = subprocess.Popen(
            ["sh", "-c", f'exec "$0" 3<&{dbus_theirs.fileno()} 4<&{tools_theirs.fileno()}', hub],
            env=dict(self.env, AGENT_COMPOSITOR_FD="3", AGENT_COMPOSITOR_TOOLS_FD="4"),
            pass_fds=(dbus_theirs.fileno(), tools_theirs.fileno()),
            stdout=self.log, stderr=subprocess.STDOUT)
        dbus_theirs.close()
        tools_theirs.close()
        self._dbus_sock = dbus_ours
        threading.Thread(target=self._dbus, daemon=True).start()
        threading.Thread(target=self._tools, daemon=True).start()

    # -- the compositor interface ------------------------------------
    def _dbus(self):
        self.ctx.push_thread_default()
        gsock = Gio.Socket.new_from_fd(self._dbus_sock.detach())
        stream = gsock.connection_factory_create_connection()
        self.conn = Gio.DBusConnection.new_sync(
            stream, Gio.dbus_generate_guid(),
            Gio.DBusConnectionFlags.AUTHENTICATION_SERVER |
            Gio.DBusConnectionFlags.DELAY_MESSAGE_PROCESSING, None, None)
        self.conn.register_object(PATH, self.node.interfaces[0], self._method, self._get, None)
        self.conn.start_message_processing()
        self.loop.run()

    def _method(self, conn, sender, path, iface, method, params, inv):
        if method == "Hello":
            inv.return_value(GLib.Variant("(ua{sv})", (1, {
                "name": GLib.Variant("s", "harness"),
                "locked": GLib.Variant("b", self.locked),
                "provider": GLib.Variant("s", "org.test.Harness")})))
        else:
            inv.return_dbus_error("org.freedesktop.DBus.Error.NotSupported", "no")

    def _get(self, conn, sender, path, iface, prop):
        if prop == "version":
            return GLib.Variant("u", 1)
        if prop == "Locked":
            return GLib.Variant("b", self.locked)
        return GLib.Variant("b", False)

    def set_locked(self, locked):
        self.locked = locked

        def emit():
            self.conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged",
                                  GLib.Variant("(sa{sv}as)", (IFACE, {
                                      "Locked": GLib.Variant("b", locked)}, [])))
            return False
        self.ctx.invoke_full(GLib.PRIORITY_DEFAULT, emit)

    # -- the compositor's tools (MCP, stateless) ------------------------
    def _send(self, obj):
        data = (json.dumps(obj) + "\n").encode()
        with self.lock:
            try:
                self.tools_ours.sendall(data)
            except OSError:
                pass

    def _result(self, rid, result):
        result = dict(result, resultType="complete")
        self._send({"jsonrpc": "2.0", "id": rid, "result": result})

    def _tools(self):
        f = self.tools_ours.makefile("rb")
        for raw in f:
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            method, rid = msg.get("method"), msg.get("id")
            params = msg.get("params", {})
            if method == "notifications/cancelled":
                self.cancelled.append(params.get("requestId"))
                continue
            if rid is None:
                continue
            if method == "server/discover":
                self._result(rid, {"supportedVersions": ["2026-07-28"],
                                   "capabilities": {"tools": {}}, "ttlMs": 0, "cacheScope": "private"})
            elif method == "subscriptions/listen":
                pass
            elif method == "tools/list":
                tools = [{"name": n, "description": "x", "inputSchema": {"type": "object"}}
                         for n in ("sleep", "echo", "zap")]
                self._result(rid, {"tools": tools, "ttlMs": 0, "cacheScope": "private"})
            elif method == "tools/call":
                name, args = params.get("name"), params.get("arguments", {})
                token = params.get("_meta", {}).get("progressToken")
                self.calls.append((name, args))
                if name == "sleep":
                    threading.Thread(target=self._sleep, args=(rid, args, token), daemon=True).start()
                else:
                    self._result(rid, {"content": [{"type": "text", "text": json.dumps(args)}]})
            else:
                self._send({"jsonrpc": "2.0", "id": rid,
                            "error": {"code": -32601, "message": "no"}})

    def _sleep(self, rid, args, token):
        with self.lock:
            self.running += 1
            self.max_running = max(self.max_running, self.running)
        end = time.time() + args.get("ms", 0) / 1000
        every = args.get("progress_ms")
        n = 0
        while time.time() < end:
            step = min(end - time.time(), (every or 50) / 1000)
            time.sleep(max(step, 0))
            if every and token is not None and time.time() < end:
                n += 1
                self._send({"jsonrpc": "2.0", "method": "notifications/progress",
                            "params": {"progressToken": token, "progress": n}})
        with self.lock:
            self.running -= 1
        self._result(rid, {"content": [{"type": "text", "text": "slept"}]})

    def drop_tools(self):
        """The provider goes away (its socket closed)."""
        try:
            self.tools_ours.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    # -- an agent, on the hub's socket ---------------------------------
    def agent(self):
        path = os.path.join(self.run_dir, "agents", "tools", "agent")
        for _ in range(200):
            if os.path.exists(path):
                break
            time.sleep(0.02)
        return Agent(path)

    def audit(self):
        path = os.path.join(self.state_dir, "agents", "tools", "audit.log")
        try:
            with open(path) as f:
                return [json.loads(l) for l in f if l.strip()]
        except OSError:
            return []

    def hub_log(self):
        with open(os.path.join(self.work, "hub.log"), errors="replace") as f:
            return f.read()

    def stop(self):
        if self.hub.poll() is None:
            self.hub.terminate()
            try:
                self.hub.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.hub.kill()
        self.loop.quit()
        self.log.close()


META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {}}


class Agent:
    """A stateless agent speaking straight to the hub's socket."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rb")
        self.s.sendall(b'{"agent-tools": 1, "label": "harness-agent"}\n')
        self.handshake = json.loads(self.f.readline())
        self.messages = []
        self.cond = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.f:
            with self.cond:
                self.messages.append(json.loads(raw))
                self.cond.notify_all()

    def send(self, obj):
        self.s.sendall((json.dumps(obj) + "\n").encode())

    def call(self, rid, name, args=None, progress_token=None):
        meta = dict(META)
        if progress_token is not None:
            meta["progressToken"] = progress_token
        self.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                   "params": {"name": name, "arguments": args or {}, "_meta": meta}})

    def cancel(self, rid):
        self.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                   "params": {"requestId": rid}})

    def wait(self, pred, timeout=10):
        end = time.time() + timeout
        with self.cond:
            while not pred(self.messages):
                left = end - time.time()
                if left <= 0:
                    return False
                self.cond.wait(left)
        return True

    def answer(self, rid, timeout=10):
        found = []

        def has(msgs):
            found[:] = [m for m in msgs if m.get("id") == rid and "method" not in m]
            return bool(found)
        return found[0] if self.wait(has, timeout) else None

    def close(self):
        # (shut down, not only closed: the reader's file object holds the
        # socket open too)
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.s.close()
