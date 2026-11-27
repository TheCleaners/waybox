#!/usr/bin/env python3
"""A stand-in xdg-desktop-portal with only Location, for tests, on the bus
DBUS_SESSION_BUS_ADDRESS names (a private one). It answers as the portal
does: CreateSession, then Start, then a Response on the request and a
LocationUpdated on the session (48.86, 2.35: Paris); with "deny", a
Response of 1 (the user said no) and no place.

Usage: fake_location_portal.py [deny]
"""
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

DENY = len(sys.argv) > 1 and sys.argv[1] == "deny"
PATH = "/org/freedesktop/portal/desktop"
XML = """
<node>
  <interface name="org.freedesktop.portal.Location">
    <method name="CreateSession">
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <method name="Start">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <signal name="LocationUpdated">
      <arg type="o" name="session_handle"/>
      <arg type="a{sv}" name="location"/>
    </signal>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
"""


def sender_part(sender):
    return sender[1:].replace(".", "_")


def main():
    loop = GLib.MainLoop()
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]

    def call(conn, sender, path, iface, method, params, inv):
        opts = params.unpack()[-1]
        if method == "CreateSession":
            session = f"{PATH}/session/{sender_part(sender)}/{opts['session_handle_token']}"
            print(f"create accuracy={opts.get('accuracy')}", flush=True)
            inv.return_value(GLib.Variant("(o)", (session,)))
        elif method == "Start":
            session = params.unpack()[0]
            request = f"{PATH}/request/{sender_part(sender)}/{opts['handle_token']}"
            inv.return_value(GLib.Variant("(o)", (request,)))

            def answer():
                conn.emit_signal(sender, request, "org.freedesktop.portal.Request", "Response",
                                 GLib.Variant("(ua{sv})", (1 if DENY else 0, {})))
                if not DENY:
                    conn.emit_signal(sender, PATH, "org.freedesktop.portal.Location",
                                     "LocationUpdated", GLib.Variant("(oa{sv})", (session, {
                                         "Latitude": GLib.Variant("d", 48.86),
                                         "Longitude": GLib.Variant("d", 2.35),
                                         "Accuracy": GLib.Variant("d", 500.0)})))
                print("answered", flush=True)
                return False
            GLib.timeout_add(200, answer)

    def get_property(conn, sender, path, iface, name):
        return GLib.Variant("u", 1)

    bus.register_object(PATH, info, call, get_property, None)

    # (The session's Close isn't answered: wbconf doesn't wait for it.)
    Gio.bus_own_name_on_connection(bus, "org.freedesktop.portal.Desktop",
                                   Gio.BusNameOwnerFlags.NONE, None, None)
    print("ready", flush=True)
    loop.run()


if __name__ == "__main__":
    main()
