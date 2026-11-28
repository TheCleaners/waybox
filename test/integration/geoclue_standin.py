#!/usr/bin/env python3
"""A stand-in for GeoClue's manager, on the bus DBUS_SYSTEM_BUS_ADDRESS
names (a private one in the tests): it owns org.freedesktop.GeoClue2 and
takes AddAgent for the IDs in GEOCLUE_ALLOW (comma-separated), refusing
others as GeoClue does (AccessDenied).

Lines on stdout: "ready", "agent ID SENDER" (each AddAgent taken),
"refused ID", "answer APP AUTHORIZED LEVEL" (or "error APP MESSAGE").
Commands on stdin: "ask APP LEVEL" (AuthorizeApp on the last agent).
"""
import os
import sys
import threading

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MANAGER_XML = """<node><interface name='org.freedesktop.GeoClue2.Manager'>
<method name='AddAgent'><arg name='id' type='s' direction='in'/></method>
</interface></node>"""

allowed = set(filter(None, os.environ.get("GEOCLUE_ALLOW", "").split(",")))
agent = {"sender": None}
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)


def out(line):
    print(line, flush=True)


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "AddAgent":
        (agent_id,) = params.unpack()
        if agent_id not in allowed:
            out(f"refused {agent_id}")
            invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied",
                                         f"'{agent_id}' not allowed")
            return
        agent["sender"] = sender
        out(f"agent {agent_id} {sender}")
        invocation.return_value(None)


node = Gio.DBusNodeInfo.new_for_xml(MANAGER_XML)
bus.register_object("/org/freedesktop/GeoClue2/Manager", node.interfaces[0], on_call, None, None)


def owned(*_):
    out("ready")


Gio.bus_own_name_on_connection(bus, "org.freedesktop.GeoClue2", Gio.BusNameOwnerFlags.NONE,
                               owned, None)


def ask(app, level):
    if agent["sender"] is None:
        out(f"error {app} no agent")
        return

    def done(conn, res):
        try:
            r = conn.call_finish(res).unpack()
            out(f"answer {app} {str(r[0]).lower()} {r[1]}")
        except GLib.Error as e:
            out(f"error {app} {e.message}")
    bus.call(agent["sender"], "/org/freedesktop/GeoClue2/Agent", "org.freedesktop.GeoClue2.Agent",
             "AuthorizeApp", GLib.Variant("(su)", (app, int(level))), GLib.VariantType("(bu)"),
             Gio.DBusCallFlags.NONE, 60000, None, done)


def read_commands():
    for line in sys.stdin:
        parts = line.split()
        if len(parts) == 3 and parts[0] == "ask":
            GLib.idle_add(lambda a=parts[1], l=parts[2]: (ask(a, l), False)[1])
    GLib.idle_add(loop.quit)


loop = GLib.MainLoop()
threading.Thread(target=read_commands, daemon=True).start()
loop.run()
