#!/usr/bin/env python3
"""Prototype surface: what a Waybar custom module would run. Follows the
hub's state and prints one Waybar JSON line per change:

  {"text": "", "class": "idle"}                 nothing going on
  {"text": "AI", "class": "active", ...}        a call is running
  {"text": "AI ●", "class": "watching", ...}    capture or input is active
  tooltip: agents, grants, the last call

In Waybar:
  "custom/agent": {"exec": "surface.py", "return-type": "json",
                   "on-click": "surface.py --stop",
                   "on-click-right": "surface.py --revoke-all"}

Run on a private bus only (dbus-run-session); see demo.sh.
"""
import json
import sys

from gi.repository import Gio, GLib

NAME, PATH, IFACE = "org.waybox.AgentHub1", "/org/waybox/AgentHub1", "org.waybox.AgentHub1"
last = {}


def main():
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    if len(sys.argv) > 1:
        method = {"--stop": "Stop", "--revoke-all": "RevokeAll"}[sys.argv[1]]
        conn.call_sync(NAME, PATH, IFACE, method, None, None, Gio.DBusCallFlags.NONE, 5000, None)
        return
    hub = Gio.DBusProxy.new_sync(conn, Gio.DBusProxyFlags.NONE, None, NAME, PATH, IFACE, None)

    def show(*_):
        agents = hub.get_cached_property("Agents").unpack() if hub.get_cached_property("Agents") else []
        grants = hub.get_cached_property("Grants").unpack() if hub.get_cached_property("Grants") else []
        act = hub.get_cached_property("Active").unpack() if hub.get_cached_property("Active") else []
        watching = hub.get_cached_property("Watching")
        watching = watching.unpack() if watching else False
        lines = [f"{len(agents)} assistant(s): " + ", ".join(
            f"{a['client']} ({a['profile']})" for a in agents)]
        lines += [f"granted: {g['tool'] or g['sensitivity']} to {g['profile']}"
                  + (" for the session" if g["until"] == 0 else " for a while") for g in grants]
        if last:
            lines.append(f"last: {last['tool']} → {last['outcome']}")
        cls = "watching" if watching else "active" if act else "idle" if not grants else "granted"
        text = {"watching": "AI ●", "active": "AI", "granted": "AI", "idle": ""}[cls]
        print(json.dumps({"text": text, "class": cls, "tooltip": "\n".join(lines)}), flush=True)

    def on_signal(proxy, sender, signal, params):
        if signal == "CallEnded":
            last.clear()
            last.update(params.unpack()[0])
            show()

    hub.connect("g-properties-changed", show)
    hub.connect("g-signal", on_signal)
    show()
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
