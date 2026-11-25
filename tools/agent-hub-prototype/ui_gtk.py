#!/usr/bin/env python3
"""A UI provider prototype: org.waybox.AgentUI1 drawn with GTK 4
(doc/dbus/org.waybox.AgentUI1.xml). Ask, Choose and Form (from MCP
elicitation's schema), each in a window that is plainly agent UI: a banner
with the "from" claim, and a frame unlike system dialogs.

  ui_gtk.py   (on the session bus as org.waybox.AgentUI.Gtk)
"""
import itertools
import json
import os

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
XML = os.path.join(HERE, "..", "..", "doc", "dbus", "org.waybox.AgentUI1.xml")
NAME = "org.waybox.AgentUI.Gtk"
CSS = b"""
window.agent-ui { border: 3px dashed #c08020; }
.agent-banner { background: #3a2a10; color: #ffd890; padding: 6px 10px; }
.agent-warning { color: #c03020; font-weight: bold; }
"""
REQUEST_XML = """<node><interface name="org.freedesktop.portal.Request">
<method name="Close"/></interface></node>"""

ids = itertools.count(1)
conn = None
app = None


class Request:
    def __init__(self, sender):
        self.path = f"/org/waybox/AgentUI1/request/{next(ids)}"
        self.sender = sender
        self.window = None
        self.done = False
        info = Gio.DBusNodeInfo.new_for_xml(REQUEST_XML).interfaces[0]
        self.reg = conn.register_object(self.path, info, self.on_call, None, None)

    def on_call(self, c, sender, path, iface, method, params, inv):
        inv.return_value(None)
        self.respond(2, {})  # Close: the hub withdrew it

    def respond(self, code, results):
        if self.done:
            return
        self.done = True
        conn.emit_signal(self.sender, self.path, "org.freedesktop.portal.Request", "Response",
                         GLib.Variant("(ua{sv})", (code, results)))
        conn.unregister_object(self.reg)
        if self.window is not None:
            self.window.destroy()


def frame(req, title, options):
    """The agent-UI frame: banner with the claim, then the content box."""
    win = Gtk.Window(application=app, title=f"Assistant: {title}")
    win.add_css_class("agent-ui")
    win.set_default_size(420, -1)
    # Not modal, no transient parent: it never takes over another window.
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    banner = Gtk.Label(label=options.get("from", "An assistant") + " asks:", xalign=0)
    banner.add_css_class("agent-banner")
    box.append(banner)
    inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin_start=12,
                    margin_end=12, margin_bottom=12)
    box.append(inner)
    win.set_child(box)
    req.window = win
    win.connect("close-request", lambda *_: (req.respond(1, {}), False)[1])
    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", lambda c, kv, code, st:
                 (req.respond(1, {}), True)[1] if kv == Gdk.KEY_Escape else False)
    win.add_controller(keys)
    timeout = options.get("timeout", 0)
    if timeout:
        GLib.timeout_add_seconds(timeout, lambda: (req.respond(2, {}), False)[1])
    return win, inner


def buttons(inner, req, on_ok, ok_label="Send to assistant"):
    row = Gtk.Box(spacing=8, halign=Gtk.Align.END)
    cancel = Gtk.Button(label="Cancel")
    ok = Gtk.Button(label=ok_label)
    ok.add_css_class("suggested-action")
    cancel.connect("clicked", lambda *_: req.respond(1, {}))
    ok.connect("clicked", lambda *_: on_ok())
    row.append(cancel)
    row.append(ok)
    inner.append(row)
    # Enter submits (once the brief pause is over), Escape cancels.
    win = inner.get_root()
    if win is not None:
        win.set_default_widget(ok)
    # Input ignored briefly after showing (a click meant elsewhere can't land).
    for b in (ok, cancel):
        b.set_sensitive(False)
    GLib.timeout_add(500, lambda: ([b.set_sensitive(True) for b in (ok, cancel)], False)[1])
    return ok


def ask(sender, prompt, options):
    req = Request(sender)
    win, inner = frame(req, "a question", options)
    inner.append(Gtk.Label(label=prompt, xalign=0, wrap=True))
    if options.get("notice"):
        buttons(inner, req, lambda: req.respond(0, {}), "OK")
        win.present()
        return req.path
    entry = Gtk.PasswordEntry() if options.get("secret") else Gtk.Entry()
    entry.props.activates_default = True
    if options.get("default"):
        entry.set_text(options["default"])
    inner.append(entry)
    if options.get("secret"):
        warn = Gtk.Label(label="This answer goes to the assistant, not to the system.", xalign=0)
        warn.add_css_class("agent-warning")
        inner.append(warn)
    buttons(inner, req, lambda: req.respond(0, {"text": GLib.Variant("s", entry.get_text())}))
    win.present()
    return req.path


def choose(sender, prompt, items, options):
    req = Request(sender)
    win, inner = frame(req, "a choice", options)
    inner.append(Gtk.Label(label=prompt, xalign=0, wrap=True))
    multiple = options.get("multiple", False)
    checks = []
    group = None
    for item_id, label in items:
        b = Gtk.CheckButton(label=label)
        if not multiple:
            if group is None:
                group = b
            else:
                b.set_group(group)
        checks.append((item_id, b))
        inner.append(b)
    buttons(inner, req, lambda: req.respond(0, {"chosen": GLib.Variant(
        "as", [i for i, b in checks if b.get_active()])}))
    win.present()
    return req.path


def form(sender, title, schema_text, options):
    """MCP elicitation's restricted schema: a flat object of string, number,
    integer, boolean and enum properties."""
    req = Request(sender)
    win, inner = frame(req, title, options)
    inner.append(Gtk.Label(label=title, xalign=0, wrap=True))
    try:
        schema = json.loads(schema_text)
    except ValueError:
        schema = {}
    props = schema.get("properties", {}) if isinstance(schema, dict) else {}
    required = set(schema.get("required", [])) if props else set()
    grid = Gtk.Grid(column_spacing=8, row_spacing=6)
    read = {}
    for row, (key, p) in enumerate(list(props.items())[:20]):
        kind = p.get("type")
        label = Gtk.Label(label=(p.get("title") or key) + (" *" if key in required else ""), xalign=1)
        grid.attach(label, 0, row, 1, 1)
        if "enum" in p:
            names = p.get("enumNames", p["enum"])
            w = Gtk.DropDown.new_from_strings([str(n) for n in names])
            if p.get("default") in p["enum"]:
                w.set_selected(p["enum"].index(p["default"]))
            read[key] = (lambda w=w, p=p: p["enum"][w.get_selected()])
        elif kind == "boolean":
            w = Gtk.Switch(active=bool(p.get("default", False)), halign=Gtk.Align.START)
            read[key] = (lambda w=w: w.get_active())
        elif kind in ("number", "integer"):
            w = Gtk.SpinButton.new_with_range(p.get("minimum", -1e9), p.get("maximum", 1e9),
                                              1 if kind == "integer" else 0.1)
            w.set_digits(0 if kind == "integer" else 2)
            w.set_value(p.get("default", 0))
            read[key] = (lambda w=w, k=kind: int(w.get_value()) if k == "integer" else w.get_value())
        else:
            w = Gtk.PasswordEntry() if p.get("format") == "password" else Gtk.Entry()
            w.props.activates_default = True
            if p.get("default"):
                w.set_text(str(p["default"]))
            read[key] = (lambda w=w: w.get_text())
        if p.get("description"):
            w.set_tooltip_text(p["description"])
        grid.attach(w, 1, row, 1, 1)
    inner.append(grid)
    if any(p.get("format") == "password" for p in props.values()):
        warn = Gtk.Label(label="Passwords here go to the assistant, not to the system.", xalign=0)
        warn.add_css_class("agent-warning")
        inner.append(warn)
    err = Gtk.Label(xalign=0)
    err.add_css_class("agent-warning")
    inner.append(err)

    def ok():
        values = {k: f() for k, f in read.items()}
        missing = [k for k in required if values.get(k) in ("", None)]
        if missing:
            err.set_text("Required: " + ", ".join(missing))
            return
        req.respond(0, {"values": GLib.Variant("s", json.dumps(values))})
    buttons(inner, req, ok)
    win.present()
    return req.path


def on_call(c, sender, path, iface, method, params, inv):
    # Always answer: a provider that fails silently leaves the hub waiting.
    try:
        dispatch(sender, method, params, inv)
    except Exception as e:  # noqa: BLE001 (prototype: report anything)
        inv.return_dbus_error("org.waybox.AgentUI1.Error.Failed", f"{type(e).__name__}: {e}")


def dispatch(sender, method, params, inv):
    args = params.unpack()
    if method == "Ask":
        p = ask(sender, *args)
    elif method == "Choose":
        p = choose(sender, *args)
    elif method == "Form":
        p = form(sender, *args)
    else:
        inv.return_dbus_error("org.freedesktop.DBus.Error.NotSupported", method)
        return
    inv.return_value(GLib.Variant("(o)", (p,)))


def on_get(c, sender, path, iface, name):
    return GLib.Variant("as", ["ask", "choose", "form"])


def activate(a):
    global conn
    a.hold()
    css = Gtk.CssProvider()
    css.load_from_data(CSS)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    info = Gio.DBusNodeInfo.new_for_xml(open(XML).read()).interfaces[0]
    conn.register_object("/org/waybox/AgentUI1", info, on_call, on_get, None)
    Gio.bus_own_name_on_connection(conn, NAME, Gio.BusNameOwnerFlags.NONE,
                                   lambda *_: print("ui: ready", flush=True), None)


def main():
    global app
    app = Gtk.Application(application_id="org.waybox.AgentUIGtk",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect("activate", activate)
    app.run([])


if __name__ == "__main__":
    main()
