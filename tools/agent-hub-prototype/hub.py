#!/usr/bin/env python3
"""Prototype of the agent hub's D-Bus side (doc/agent-tools.md,
doc/dbus/org.waybox.AgentHub1.xml). Not the hub: no MCP, no providers.
Its state is driven by a debug interface, to try the D-Bus shapes:

  org.waybox.AgentHub1.Debug.Connect(profile, client) -> agent id
  org.waybox.AgentHub1.Debug.Call(agent, provider, tool, sensitivity) -> outcome
  org.waybox.AgentHub1.Debug.AddProvider(id, name, highest)
  org.waybox.AgentHub1.Debug.Approve(id)  (stands in for the compositor's
      confirmation: approving isn't on the public interface)

Levels (the "trusted" preset): read/content/act allow; destroy/exec/capture
ask; input off. "ask" calls org.freedesktop.impl.portal.Access.AccessDialog
on $CONSENT_BUS_NAME (default org.freedesktop.impl.portal.desktop.waybox),
with a scope choice: once, this session, 30 minutes.

Run on a private bus only (dbus-run-session); see demo.sh.
"""
import itertools
import os
import sys
import time

from gi.repository import Gio, GLib

HERE = os.path.dirname(os.path.abspath(__file__))
XML = os.path.join(HERE, "..", "..", "doc", "dbus", "org.waybox.AgentHub1.xml")
DEBUG_XML = """<node><interface name="org.waybox.AgentHub1.Debug">
  <method name="Connect"><arg name="profile" type="s" direction="in"/>
    <arg name="client" type="s" direction="in"/><arg name="agent" type="s" direction="out"/></method>
  <method name="Call"><arg name="agent" type="s" direction="in"/><arg name="provider" type="s" direction="in"/>
    <arg name="tool" type="s" direction="in"/><arg name="sensitivity" type="s" direction="in"/>
    <arg name="outcome" type="s" direction="out"/></method>
  <method name="AddProvider"><arg name="id" type="s" direction="in"/><arg name="name" type="s" direction="in"/>
    <arg name="highest" type="s" direction="in"/></method>
  <method name="Approve"><arg name="id" type="s" direction="in"/></method>
</interface></node>"""

PATH = "/org/waybox/AgentHub1"
IFACE = "org.waybox.AgentHub1"
LEVELS = {"read": "allow", "content": "allow", "act": "allow", "destroy": "ask",
          "exec": "ask", "capture": "ask", "input": "off"}
CONSENT = os.environ.get("CONSENT_BUS_NAME", "org.freedesktop.impl.portal.desktop.waybox")

ids = itertools.count(1)
agents, providers, grants, active, log = {}, {}, {}, {}, []
conn = None


def vardict(d):
    out = {}
    for k, v in d.items():
        if isinstance(v, bool):
            out[k] = GLib.Variant("b", v)
        elif isinstance(v, int):
            out[k] = GLib.Variant("x" if k in ("since", "until", "time") else "u", v)
        else:
            out[k] = GLib.Variant("s", str(v))
    return out


def records(table):
    return GLib.Variant("aa{sv}", [vardict(r) for r in table.values()])


def prop(name):
    if name == "Version":
        return GLib.Variant("u", 1)
    if name == "Watching":
        return GLib.Variant("b", any(a["sensitivity"] in ("capture", "input")
                                     for a in active.values()))
    return records({"Agents": agents, "Providers": providers, "Grants": grants,
                    "Active": active}[name])


def changed(*names):
    conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                     GLib.Variant("(sa{sv}as)", (IFACE, {n: prop(n) for n in names}, [])))


def emit(signal, rec):
    conn.emit_signal(None, PATH, IFACE, signal, GLib.Variant("(a{sv})", (vardict(rec),)))


def granted(agent, provider, tool, sensitivity):
    now = int(time.time())
    for g in grants.values():
        if (g["profile"] == agents[agent]["profile"] and g["sensitivity"] == sensitivity
                and g["provider"] in ("", provider) and g["tool"] in ("", tool)
                and (g["until"] == 0 or g["until"] > now)):
            return True
    return False


def ask(agent, provider, tool, sensitivity):
    """The consent dialog, through the Access backend. Returns the scope
    chosen ("once", "session", "30m") or None if denied/unavailable."""
    a = agents[agent]
    choices = GLib.Variant("a(ssa(ss)s)", [
        ("scope", "Allow", [("once", "Just this once"), ("session", "For this session"),
                            ("30m", "For 30 minutes")], "once")])
    handle = f"/org/freedesktop/portal/desktop/request/hub/{next(ids)}"
    try:
        res = conn.call_sync(
            CONSENT, "/org/freedesktop/portal/desktop", "org.freedesktop.impl.portal.Access",
            "AccessDialog",
            GLib.Variant("(osssssa{sv})", (
                handle, "",  # no app id: the hub, not a sandboxed app
                "",          # no parent window: the compositor places it
                f"An assistant calling itself \u201c{a['client']}\u201d wants to use "
                f"{providers.get(provider, {}).get('name', provider)}: {tool}",
                f"Profile “{a['profile']}” · {sensitivity}",
                f"The assistant asks to run {tool} ({sensitivity}). Choose how long to allow it.",
                {"grant_label": GLib.Variant("s", "Allow"),
                 "deny_label": GLib.Variant("s", "Deny"),
                 "icon": GLib.Variant("s", "dialog-password-symbolic"),
                 "choices": choices})),
            GLib.VariantType("(ua{sv})"), Gio.DBusCallFlags.NONE, 120000, None)
    except GLib.Error as e:
        print(f"hub: no consent backend ({e.message}): as denied", file=sys.stderr)
        return None
    response, results = res.unpack()
    if response != 0:
        return None
    scope = dict(results.get("choices", [])).get("scope", "once")
    if scope in ("session", "30m"):
        gid = f"g{next(ids)}"
        grants[gid] = {"id": gid, "profile": a["profile"], "provider": provider, "tool": tool,
                       "sensitivity": sensitivity,
                       "until": 0 if scope == "session" else int(time.time()) + 1800}
        changed("Grants")
    return scope


def call(agent, provider, tool, sensitivity):
    if agent not in agents:
        return "refused: no such agent"
    if providers.get(provider, {}).get("state") != "approved":
        outcome = "refused"
    else:
        level = LEVELS.get(sensitivity, "off")
        if level == "ask" and not granted(agent, provider, tool, sensitivity):
            outcome = None if ask(agent, provider, tool, sensitivity) else "denied"
        elif level == "off":
            outcome = "refused"
        else:
            outcome = None
    cid = f"c{next(ids)}"
    rec = {"call": cid, "agent": agent, "profile": agents[agent]["profile"],
           "provider": provider, "tool": tool, "sensitivity": sensitivity}
    if outcome is None:
        emit("CallStarted", rec)
        active[cid] = dict(rec, since=int(time.time()))
        changed("Active", "Watching")
        # (the tool would run here)
        del active[cid]
        changed("Active", "Watching")
        outcome = "done"
    end = dict(rec, outcome=outcome, time=int(time.time()))
    log.append(end)
    emit("CallEnded", end)
    return outcome


def on_hub(c, sender, path, iface, method, params, inv):
    args = params.unpack()
    if method == "RecentCalls":
        n = min(args[0], 100)
        inv.return_value(GLib.Variant("(aa{sv})", ([vardict(r) for r in log[-n:]] if n else [],)))
        return
    if method == "Revoke":
        grants.pop(args[0], None)
        changed("Grants")
    elif method in ("RevokeAll", "Stop"):
        grants.clear()
        changed("Grants")
        if method == "Stop":
            for cid, rec in list(active.items()):
                emit("CallEnded", dict(rec, outcome="cancelled"))
            active.clear()
            changed("Active", "Watching")
    elif method == "RefuseProvider":
        if args[0] in providers:
            providers[args[0]]["state"] = "refused"
            changed("Providers")
    inv.return_value(None)


def on_debug(c, sender, path, iface, method, params, inv):
    args = params.unpack()
    if method == "Connect":
        aid = f"a{next(ids)}"
        agents[aid] = {"id": aid, "profile": args[0], "client": args[1], "since": int(time.time())}
        changed("Agents")
        inv.return_value(GLib.Variant("(s)", (aid,)))
    elif method == "AddProvider":
        providers[args[0]] = {"id": args[0], "name": args[1], "state": "pending",
                              "connected": True, "tools": 1, "highest": args[2]}
        changed("Providers")
        inv.return_value(None)
    elif method == "Call":
        inv.return_value(GLib.Variant("(s)", (call(*args),)))
    elif method == "Approve":
        if args[0] in providers:
            providers[args[0]]["state"] = "approved"
            changed("Providers")
        inv.return_value(None)


def on_get(c, sender, path, iface, name):
    return prop(name)


def main():
    global conn
    loop = GLib.MainLoop()
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    hub_info = Gio.DBusNodeInfo.new_for_xml(open(XML).read()).interfaces[0]
    dbg_info = Gio.DBusNodeInfo.new_for_xml(DEBUG_XML).interfaces[0]
    conn.register_object(PATH, hub_info, on_hub, on_get, None)
    conn.register_object(PATH, dbg_info, on_debug, None, None)
    providers["org.waybox.Waybox"] = {"id": "org.waybox.Waybox", "name": "Waybox",
                                      "state": "approved", "connected": True, "tools": 12,
                                      "highest": "exec"}
    Gio.bus_own_name_on_connection(conn, IFACE, Gio.BusNameOwnerFlags.NONE,
                                   lambda *a: print("hub: ready", flush=True), None)
    loop.run()


if __name__ == "__main__":
    main()
