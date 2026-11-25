#!/usr/bin/env python3
"""Prototype consent backend: org.freedesktop.impl.portal.Access, as Waybox's
portal backend would offer it (drawn in the compositor; here, printed).
Answers from $ANSWER: "deny", or "grant:<scope>" (once, session, 30m).

Run on a private bus only (dbus-run-session); see demo.sh.
"""
import os

from gi.repository import Gio, GLib

ACCESS_XML = "/usr/share/dbus-1/interfaces/org.freedesktop.impl.portal.Access.xml"
NAME = os.environ.get("CONSENT_BUS_NAME", "org.freedesktop.impl.portal.desktop.waybox")


def on_call(c, sender, path, iface, method, params, inv):
    handle, app_id, parent, title, subtitle, body, options = params.unpack()
    print("consent: ┌──────────────────────────────────────────────")
    print(f"consent: │ {title}")
    print(f"consent: │ {subtitle}")
    print(f"consent: │ {body}")
    for cid, label, opts, default in options.get("choices", []):
        print(f"consent: │ {label}: " + " / ".join(
            ("[" + l + "]" if i == default else l) for i, l in opts))
    print(f"consent: │   ({options.get('deny_label', 'Deny')})  "
          f"({options.get('grant_label', 'Grant')})")
    answer = os.environ.get("ANSWER", "deny")
    print(f"consent: └─ user chose: {answer}", flush=True)
    if answer.startswith("grant"):
        scope = answer.split(":", 1)[1] if ":" in answer else "once"
        inv.return_value(GLib.Variant("(ua{sv})", (0, {
            "choices": GLib.Variant("a(ss)", [("scope", scope)])})))
    else:
        inv.return_value(GLib.Variant("(ua{sv})", (1, {})))


def main():
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    info = Gio.DBusNodeInfo.new_for_xml(open(ACCESS_XML).read()).interfaces[0]
    conn.register_object("/org/freedesktop/portal/desktop", info, on_call, None, None)
    Gio.bus_own_name_on_connection(conn, NAME, Gio.BusNameOwnerFlags.NONE,
                                   lambda *a: print("consent: ready", flush=True), None)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
