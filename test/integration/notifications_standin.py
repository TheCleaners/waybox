#!/usr/bin/env python3
"""A stand-in notification daemon (org.freedesktop.Notifications) on the
session bus DBUS_SESSION_BUS_ADDRESS names (a private one in the tests).
NOTIFY_ACTIONS=0: it doesn't advertise the "actions" capability (as mako
without a menu).

Lines on stdout: "ready", "notify ID SUMMARY|BODY|ACTION,LABEL,..." (each
Notify), "closed ID" (CloseNotification).
Commands on stdin: "invoke ID KEY" (the action chosen, then the
notification closed, as daemons do), "dismiss ID" (closed by the user).
"""
import os
import sys
import threading

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

XML = """<node><interface name='org.freedesktop.Notifications'>
<method name='GetCapabilities'><arg type='as' direction='out'/></method>
<method name='Notify'>
<arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>
<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>
<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='out'/>
</method>
<method name='CloseNotification'><arg type='u' direction='in'/></method>
<method name='GetServerInformation'>
<arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/>
<arg type='s' direction='out'/></method>
<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>
<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>
</interface></node>"""

PATH = "/org/freedesktop/Notifications"
IFACE = "org.freedesktop.Notifications"
actions = os.environ.get("NOTIFY_ACTIONS", "1") != "0"
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
state = {"next": 1}


def out(line):
    print(line, flush=True)


def emit(name, args):
    bus.emit_signal(None, PATH, IFACE, name, args)


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "GetCapabilities":
        invocation.return_value(GLib.Variant("(as)", (["body", "actions"] if actions else ["body"],)))
    elif method == "GetServerInformation":
        invocation.return_value(GLib.Variant("(ssss)", ("standin", "test", "1", "1.2")))
    elif method == "Notify":
        app, replaces, icon, summary, body, acts, hints, timeout = params.unpack()
        nid = replaces or state["next"]
        state["next"] += 1
        out(f"notify {nid} {summary}|{body}|{','.join(acts)}")
        invocation.return_value(GLib.Variant("(u)", (nid,)))
    elif method == "CloseNotification":
        (nid,) = params.unpack()
        out(f"closed {nid}")
        emit("NotificationClosed", GLib.Variant("(uu)", (nid, 3)))
        invocation.return_value(None)


node = Gio.DBusNodeInfo.new_for_xml(XML)
bus.register_object(PATH, node.interfaces[0], on_call, None, None)
Gio.bus_own_name_on_connection(bus, IFACE, Gio.BusNameOwnerFlags.NONE,
                               lambda *_: out("ready"), None)


def command(parts):
    if parts[0] == "invoke":
        emit("ActionInvoked", GLib.Variant("(us)", (int(parts[1]), parts[2])))
        emit("NotificationClosed", GLib.Variant("(uu)", (int(parts[1]), 2)))
    elif parts[0] == "dismiss":
        emit("NotificationClosed", GLib.Variant("(uu)", (int(parts[1]), 2)))
    return False


def read_commands():
    for line in sys.stdin:
        parts = line.split()
        if parts:
            GLib.idle_add(command, parts)
    GLib.idle_add(loop.quit)


loop = GLib.MainLoop()
threading.Thread(target=read_commands, daemon=True).start()
loop.run()
