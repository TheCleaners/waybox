"""Waybox's scripting socket from Python.

Waybox listens on ``$XDG_RUNTIME_DIR/waybox-$WAYLAND_DISPLAY.sock`` (and
exports it as ``$WAYBOX_SOCK``) when rc.xml has
``<waybox><ipc enabled="yes"/></waybox>``. Only the user running Waybox can
connect. One JSON object per line each way.

    import waybox

    with waybox.connect() as wb:
        for w in wb.windows():
            print(w["app_id"], w["title"])
        wb.go_to_desktop(2)
        wb.action("Execute", command="foot")

    for event in waybox.subscribe(["window", "desktop"]):
        print(event)

Standard library only. The requests and their fields are those of
waybox(5) and ``waybox-msg``; this module adds nothing on the wire.
"""

from __future__ import annotations

import json
import os
import socket
from typing import Any, Iterator, Optional

__all__ = [
    "Connection", "WayboxError", "NotRunning", "RequestFailed",
    "connect", "subscribe", "socket_path",
]

#: The events subscribe() can ask for.
EVENTS = ("window", "desktop", "output", "config")

# Bounds on what is read from the socket, so a broken peer can't make us
# hold unbounded data.
_MAX_LINE = 16 * 1024 * 1024


class WayboxError(Exception):
    """Something went wrong talking to Waybox."""


class NotRunning(WayboxError):
    """No socket to connect to: Waybox isn't running, or its IPC is off
    (``<waybox><ipc enabled="yes"/>`` in rc.xml)."""


class RequestFailed(WayboxError):
    """Waybox answered with an error (an unknown window, the screen locked,
    an unknown action...). ``str(e)`` is Waybox's message."""


def socket_path(display: Optional[str] = None) -> str:
    """The socket's path: ``$WAYBOX_SOCK``, else the one for `display`
    (default ``$WAYLAND_DISPLAY``) in ``$XDG_RUNTIME_DIR``."""
    if display is None and os.environ.get("WAYBOX_SOCK"):
        return os.environ["WAYBOX_SOCK"]
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    display = display or os.environ.get("WAYLAND_DISPLAY")
    if not runtime or not display:
        raise NotRunning("no $WAYBOX_SOCK, and no $XDG_RUNTIME_DIR or $WAYLAND_DISPLAY "
                         "to find Waybox's socket with")
    if "/" in display:
        # An absolute WAYLAND_DISPLAY: the socket is named after its basename.
        display = os.path.basename(display)
    return os.path.join(runtime, f"waybox-{display}.sock")


class Connection:
    """A connection to Waybox's socket. Requests are answered in order;
    one connection may carry any number of them.

    `timeout` (seconds) bounds each answer; None waits forever.
    """

    def __init__(self, path: Optional[str] = None, timeout: Optional[float] = 5.0):
        self.path = path or socket_path()
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        try:
            self._sock.connect(self.path)
        except (FileNotFoundError, ConnectionRefusedError) as e:
            self._sock.close()
            raise NotRunning(f"can't connect to {self.path}: {e.strerror}. Is Waybox "
                             "running with <waybox><ipc enabled=\"yes\"/>?") from None
        self._buf = b""

    # ---- the plumbing -------------------------------------------------

    def _send(self, obj: dict) -> None:
        self._sock.sendall((json.dumps(obj) + "\n").encode())

    def _read(self) -> dict:
        while b"\n" not in self._buf:
            if len(self._buf) > _MAX_LINE:
                raise WayboxError("answer too long")
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                raise WayboxError("no answer from Waybox in time") from None
            if not chunk:
                raise WayboxError("Waybox closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        try:
            obj = json.loads(line)
        except ValueError:
            raise WayboxError(f"not JSON from Waybox: {line[:200]!r}") from None
        if not isinstance(obj, dict):
            raise WayboxError("Waybox's answer isn't a JSON object")
        return obj

    def request(self, request: str, **fields: Any) -> dict:
        """Send one request and return Waybox's answer (``{"ok": true,
        ...}``). Raises RequestFailed with Waybox's message on an error."""
        self._send(dict(fields, request=request))
        answer = self._read()
        if not answer.get("ok", False):
            raise RequestFailed(answer.get("error", "failed"))
        return answer

    # ---- what Waybox can tell ----------------------------------------

    def version(self) -> str:
        """Waybox's version (as ``waybox --version`` says)."""
        return self.request("get_version").get("version", "")

    def windows(self) -> list[dict]:
        """The windows, most recently used first: id, app_id, title, tag,
        desktop (1-based), omnipresent, focused, minimized, maximized,
        fullscreen, shaded, x, y, width, height, xwayland."""
        return self.request("get_windows").get("windows", [])

    def focused(self) -> Optional[dict]:
        """The focused window, or None."""
        for w in self.windows():
            if w.get("focused"):
                return w
        return None

    def desktops(self) -> list[dict]:
        """The desktops: number (1-based), name, current."""
        return self.request("get_desktops").get("desktops", [])

    def outputs(self) -> list[dict]:
        """The outputs (monitors): name, position, size, scale, and more."""
        return self.request("get_outputs").get("outputs", [])

    def stalls(self) -> dict:
        """What has blocked Waybox's event loop (see waybox(1))."""
        return self.request("get_stalls")

    # ---- what Waybox can do ------------------------------------------

    def identify(self, output: Optional[str] = None, ms: Optional[int] = None) -> int:
        """Show each monitor's name, large, on it (only ``output``'s, if
        given) for ``ms`` milliseconds (default 3000); how many were shown."""
        fields: dict = {}
        if output is not None:
            fields["output"] = output
        if ms is not None:
            fields["ms"] = ms
        return int(self.request("identify", **fields).get("shown", 0))

    def action(self, action: str, *, window: Optional[str] = None,
               command: Optional[str] = None, to: Optional[str | int] = None,
               direction: Optional[str] = None, wrap: Optional[bool] = None,
               follow: Optional[bool] = None) -> None:
        """Run an rc.xml action (see waybox(5)): ``action("Close")``,
        ``action("GoToDesktop", to=2)``, ``action("Execute", command="foot")``.

        `window` picks the window it acts on: an id from windows(),
        ``"app_id:foot"``, ``"title:..."``, or ``"focused"`` (the default).
        """
        fields: dict[str, Any] = {"action": action}
        for key, value in (("window", window), ("command", command),
                           ("direction", direction), ("wrap", wrap), ("follow", follow)):
            if value is not None:
                fields[key] = value
        if to is not None:
            fields["to"] = str(to)
        self.request("action", **fields)

    def focus(self, window: str) -> None:
        self.action("Focus", window=window)

    def close_window(self, window: str = "focused") -> None:
        self.action("Close", window=window)

    def go_to_desktop(self, desktop: int | str) -> None:
        """Show desktop `desktop` (1-based, or "next", "previous", "last")."""
        self.action("GoToDesktop", to=desktop)

    def send_to_desktop(self, desktop: int | str, window: str = "focused",
                        follow: bool = False) -> None:
        self.action("SendToDesktop", to=desktop, window=window, follow=follow)

    def execute(self, command: str) -> None:
        """Start a program, as Waybox's Execute does (through /bin/sh)."""
        self.action("Execute", command=command)

    def reconfigure(self) -> None:
        """Reload rc.xml, menu.xml, the theme and the scripts."""
        self.action("Reconfigure")

    # ---- events --------------------------------------------------------

    def subscribe(self, events: list[str] | tuple[str, ...] = EVENTS) -> Iterator[dict]:
        """Events as they happen, for as long as the iteration goes on:
        ``{"event": "window", "change": "new"|"close"|"focus"|"title",
        "window": {...}}``, ``{"event": "desktop", ...}`` and so on.

        The connection carries only events afterwards (use another one for
        requests); it waits for them without a timeout.
        """
        unknown = [e for e in events if e not in EVENTS]
        if unknown:
            raise ValueError(f"unknown events: {unknown} (known: {list(EVENTS)})")
        self.request("subscribe", events=list(events))
        self._sock.settimeout(None)
        while True:
            yield self._read()

    # ---- lifetime --------------------------------------------------------

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


def connect(path: Optional[str] = None, timeout: Optional[float] = 5.0) -> Connection:
    """A connection to the running Waybox (see Connection)."""
    return Connection(path, timeout)


def subscribe(events: list[str] | tuple[str, ...] = EVENTS,
              path: Optional[str] = None) -> Iterator[dict]:
    """Events from a connection of their own:
    ``for e in waybox.subscribe(["window"]): ...``."""
    conn = Connection(path, timeout=5.0)
    try:
        yield from conn.subscribe(events)
    finally:
        conn.close()
