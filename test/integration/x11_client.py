#!/usr/bin/env python3
"""Scriptable X11 client for the Xwayland integration test (python-xlib).

Reads commands on stdin, one per line:
  window NAME W H RRGGBB [PARENT]   a managed window (WM_CLASS NAME), solid
                                    colour, optionally transient for PARENT
  override NAME X Y W H RRGGBB      an override-redirect window (a "menu")
  fullscreen NAME                   ask the WM for _NET_WM_STATE_FULLSCREEN
  destroy NAME                      destroy a window

Prints "mapped NAME" when a window is mapped, and "delete NAME" when the
window manager asks it to close (WM_DELETE_WINDOW), after which the window
is destroyed.
"""
import os
import select
import sys

from Xlib import X, display, protocol


def main():
    d = display.Display()
    screen = d.screen()
    root = screen.root
    wm_protocols = d.intern_atom("WM_PROTOCOLS")
    wm_delete = d.intern_atom("WM_DELETE_WINDOW")
    net_wm_state = d.intern_atom("_NET_WM_STATE")
    net_fullscreen = d.intern_atom("_NET_WM_STATE_FULLSCREEN")
    windows = {}
    names = {}

    def out(line):
        print(line, flush=True)

    def create(name, x, y, w, h, colour, override=False, parent=None):
        win = root.create_window(
            x, y, w, h, 0, screen.root_depth, X.InputOutput, X.CopyFromParent,
            background_pixel=int(colour, 16),
            override_redirect=override,
            event_mask=X.StructureNotifyMask | X.ExposureMask)
        win.set_wm_class(name, name)
        win.set_wm_name(name)
        if not override:
            win.set_wm_protocols([wm_delete])
            win.set_wm_normal_hints(flags=0)
        if parent is not None:
            win.set_wm_transient_for(windows[parent])
        win.map()
        windows[name] = win
        names[win.id] = name
        d.flush()

    def command(line):
        parts = line.split()
        if not parts:
            return
        if parts[0] == "window":
            name, w, h, colour = parts[1], int(parts[2]), int(parts[3]), parts[4]
            parent = parts[5] if len(parts) > 5 else None
            create(name, 0, 0, w, h, colour, parent=parent)
        elif parts[0] == "override":
            name, x, y, w, h, colour = parts[1], *map(int, parts[2:6]), parts[6]
            create(name, x, y, w, h, colour, override=True)
        elif parts[0] == "fullscreen":
            win = windows[parts[1]]
            ev = protocol.event.ClientMessage(
                window=win, client_type=net_wm_state,
                data=(32, [1, net_fullscreen, 0, 1, 0]))
            root.send_event(ev, event_mask=X.SubstructureRedirectMask |
                            X.SubstructureNotifyMask)
            d.flush()
        elif parts[0] == "destroy":
            win = windows.pop(parts[1])
            win.destroy()
            d.flush()

    buf = b""
    while True:
        while d.pending_events():
            ev = d.next_event()
            if ev.type == X.MapNotify and ev.window.id in names:
                out(f"mapped {names[ev.window.id]}")
            elif ev.type == X.ClientMessage and ev.client_type == wm_protocols:
                if ev.data[1][0] == wm_delete:
                    name = names.get(ev.window.id, "?")
                    out(f"delete {name}")
                    windows.pop(name, None)
                    ev.window.destroy()
                    d.flush()
        r, _, _ = select.select([d.fileno(), sys.stdin.fileno()], [], [], 0.5)
        if sys.stdin.fileno() in r:
            chunk = os.read(sys.stdin.fileno(), 4096)
            if not chunk:
                return 0
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                command(line.decode())
        if d.fileno() in r:
            d.pending_events()  # read what arrived


if __name__ == "__main__":
    sys.exit(main())
