#!/usr/bin/env python3
"""waybox-mcp prototype: the shim an MCP client starts over stdio. It says
hello (the profile asked for, the agent's executable: its parent process),
then passes MCP lines both ways between stdio and the hub's socket.

  shim.py [--profile NAME]
"""
import os
import selectors
import socket
import sys


def parent_exe():
    try:
        return os.readlink(f"/proc/{os.getppid()}/exe")
    except OSError:
        return ""


def main():
    profile = sys.argv[sys.argv.index("--profile") + 1] if "--profile" in sys.argv else "safe"
    path = os.environ.get("AGENT_HUB_SOCKET",
                          os.path.join(os.environ["XDG_RUNTIME_DIR"], "agent-tools", "hub"))
    hub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    hub.connect(path)
    # (The real shim checks the hub's executable through its peer credentials.)
    import json
    hub.sendall((json.dumps({"hub": "hello", "profile": profile,
                             "agent_exe": parent_exe()}) + "\n").encode())
    sel = selectors.DefaultSelector()
    sel.register(sys.stdin.buffer, selectors.EVENT_READ, "in")
    sel.register(hub, selectors.EVENT_READ, "hub")
    out = sys.stdout.buffer
    while True:
        for key, _ in sel.select():
            if key.data == "in":
                data = os.read(sys.stdin.fileno(), 65536)
                if not data:
                    # The client is done sending: say so, and keep passing on
                    # the hub's answers until it closes.
                    hub.shutdown(socket.SHUT_WR)
                    sel.unregister(sys.stdin.buffer)
                    continue
                hub.sendall(data)
            else:
                data = hub.recv(65536)
                if not data:
                    return
                out.write(data)
                out.flush()


if __name__ == "__main__":
    main()
