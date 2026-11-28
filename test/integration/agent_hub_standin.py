#!/usr/bin/env python3
"""A stand-in agent hub: what Waybox starts as WAYBOX_AGENT_HUB, on the
compositor interface's socket (AGENT_COMPOSITOR_FD), as peer-to-peer D-Bus.

Writes lines to the file STANDIN_OUT names:
  connected
  hello VERSION JSON          (Hello's answer)
  prop NAME VALUE             (version, Locked, Away, AssistantsStopped)
  capture-error NAME          (Capture: not supported yet)
  changed NAME=VALUE          (PropertiesChanged)
  closed                      (the compositor closed the connection)
"""
import json
import os
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

PATH = "/org/freedesktop/AgentCompositor1"
IFACE = "org.freedesktop.AgentCompositor1"
out = open(os.environ["STANDIN_OUT"], "a", buffering=1)


def say(line):
    out.write(line + "\n")


def main():
    fd = int(os.environ["AGENT_COMPOSITOR_FD"])
    sock = Gio.Socket.new_from_fd(fd)
    stream = sock.connection_factory_create_connection()
    conn = Gio.DBusConnection.new_sync(
        stream, None, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
    say("connected")
    loop = GLib.MainLoop()

    def changed(_c, _s, _p, _i, _sig, params):
        _iface, props, _inval = params.unpack()
        for k, v in sorted(props.items()):
            say(f"changed {k}={v}")
    conn.signal_subscribe(None, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                          PATH, None, Gio.DBusSignalFlags.NONE, changed)
    conn.connect("closed", lambda *a: (say("closed"), loop.quit()))

    r = conn.call_sync(None, PATH, IFACE, "Hello",
                       GLib.Variant("(ua{sv})", (1, {"version": GLib.Variant("s", "stand-in")})),
                       GLib.VariantType("(ua{sv})"), Gio.DBusCallFlags.NONE, 5000, None)
    version, info = r.unpack()
    say(f"hello {version} {json.dumps(info, sort_keys=True)}")
    for p in ("version", "Locked", "Away", "AssistantsStopped"):
        r = conn.call_sync(None, PATH, "org.freedesktop.DBus.Properties", "Get",
                           GLib.Variant("(ss)", (IFACE, p)), None, Gio.DBusCallFlags.NONE,
                           5000, None)
        say(f"prop {p} {r.unpack()[0]}")
    try:
        conn.call_sync(None, PATH, IFACE, "Capture",
                       GLib.Variant("(a{sv}a{sv})", ({"window": GLib.Variant("s", "x")}, {})),
                       None, Gio.DBusCallFlags.NONE, 5000, None)
        say("capture-error none")
    except GLib.Error as e:
        say(f"capture-error {Gio.DBusError.get_remote_error(e)}")
    loop.run()


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001
        say(f"error {e}")
        sys.exit(1)
