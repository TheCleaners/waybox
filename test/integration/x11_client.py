#!/usr/bin/env python3
"""Scriptable X11 client for the Xwayland integration test (python-xlib).

Reads commands on stdin, one per line:
  window NAME W H RRGGBB [PARENT]   a managed window (WM_CLASS NAME), solid
                                    colour, optionally transient for PARENT
  override NAME X Y W H RRGGBB      an override-redirect window (a "menu")
  borderless NAME X Y W H RRGGBB    an undecorated window (Motif hints) at
                                    X,Y, as games make for "fullscreen"
  ortoggle NAME W H RRGGBB          a window created override-redirect and
                                    made ordinary before mapping (as Wine does)
  fullscreen NAME                   ask the WM for _NET_WM_STATE_FULLSCREEN
  activate NAME                     ask the WM to activate it
                                    (_NET_ACTIVE_WINDOW, as from a pager)
  destroy NAME                      destroy a window
  move NAME                         start an interactive move from the
                                    pointer (_NET_WM_MOVERESIZE), as a
                                    client-drawn titlebar does
  where NAME                        print "where NAME X Y": the window's
                                    position as the X server knows it
  grab NAME                         grab the keyboard (XGrabKeyboard), as
                                    VMs and remote desktops do; prints
                                    "grab NAME STATUS" (0: granted)
  ungrab                            let it go

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
    net_active = d.intern_atom("_NET_ACTIVE_WINDOW")
    net_moveresize = d.intern_atom("_NET_WM_MOVERESIZE")
    motif_hints = d.intern_atom("_MOTIF_WM_HINTS")
    windows = {}
    names = {}

    def out(line):
        print(line, flush=True)

    def create(name, x, y, w, h, colour, override=False, parent=None,
               borderless=False, toggle=False):
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
        if borderless:
            # flags=MWM_HINTS_DECORATIONS, decorations=0; keep the position.
            win.change_property(motif_hints, motif_hints, 32, [2, 0, 0, 0, 0])
            win.set_wm_normal_hints(flags=1, x=x, y=y)  # USPosition
        if toggle:
            win.set_wm_protocols([wm_delete])
            d.flush()
            win.change_attributes(override_redirect=False)
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
        elif parts[0] == "borderless":
            name, x, y, w, h, colour = parts[1], *map(int, parts[2:6]), parts[6]
            create(name, x, y, w, h, colour, borderless=True)
        elif parts[0] == "ortoggle":
            name, w, h, colour = parts[1], int(parts[2]), int(parts[3]), parts[4]
            create(name, 0, 0, w, h, colour, override=True, toggle=True)
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
        elif parts[0] == "activate":
            ev = protocol.event.ClientMessage(
                window=windows[parts[1]], client_type=net_active,
                data=(32, [2, X.CurrentTime, 0, 0, 0]))
            root.send_event(ev, event_mask=X.SubstructureRedirectMask |
                            X.SubstructureNotifyMask)
            d.flush()
        elif parts[0] == "move":
            win = windows[parts[1]]
            ptr = root.query_pointer()
            ev = protocol.event.ClientMessage(
                window=win, client_type=net_moveresize,
                data=(32, [ptr.root_x, ptr.root_y, 8, 1, 1]))  # 8 = MOVE
            root.send_event(ev, event_mask=X.SubstructureRedirectMask |
                            X.SubstructureNotifyMask)
            d.flush()
        elif parts[0] == "where":
            pos = windows[parts[1]].translate_coords(root, 0, 0)
            # translate_coords gives root's origin in the window: negate.
            out(f"where {parts[1]} {-pos.x} {-pos.y}")
        elif parts[0] == "destroy":
            win = windows.pop(parts[1])
            win.destroy()
            d.flush()
        elif parts[0] == "grab":
            win = windows[parts[1]]
            status = win.grab_keyboard(True, X.GrabModeAsync, X.GrabModeAsync, X.CurrentTime)
            d.flush()
            out(f"grab {parts[1]} {status}")
        elif parts[0] == "ungrab":
            d.ungrab_keyboard(X.CurrentTime)
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
