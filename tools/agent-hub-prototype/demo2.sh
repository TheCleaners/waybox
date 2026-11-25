#!/bin/sh
# End-to-end prototype: a scripted MCP agent -> shim -> hub -> the
# compositor (IPC) and a GTK UI provider (D-Bus), on a private headless
# Waybox and a private session bus (never the user's). Screenshots of the
# agent UI go to $OUT (default /tmp/agent-demo).
#   demo2.sh <build-dir>
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:?usage: demo2.sh <build-dir>}" && pwd)
if [ -z "${AGENT_DEMO_BUS:-}" ]; then
	AGENT_DEMO_BUS=1 exec env -u WAYLAND_DISPLAY -u DISPLAY dbus-run-session -- "$0" "$@"
fi
OUT=${OUT:-/tmp/agent-demo}
mkdir -p "$OUT"
work=$(mktemp -d /tmp/agent-demo-run-XXXXXX)
mkdir -p "$work/run" "$work/cfg/waybox"
chmod 700 "$work/run"
cat > "$work/cfg/waybox/rc.xml" <<'EOF'
<?xml version="1.0"?><openbox_config xmlns="http://openbox.org/3.4/rc">
<waybox><ipc enabled="yes"/></waybox></openbox_config>
EOF
export XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/cfg" WB_RC_XML= WB_MENU_XML=
export GSETTINGS_BACKEND=memory GDK_BACKEND=wayland GIO_USE_VFS=local NO_AT_BRIDGE=1 GSK_RENDERER=cairo
WLR_BACKENDS=headless WLR_RENDERER=pixman "$build/waybox/waybox" > "$work/waybox.log" 2>&1 &
wb=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	sock=$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' || true)
	[ -n "$sock" ] && break
	sleep 0.3
done
export WAYLAND_DISPLAY="$sock" WAYBOX_SOCK="$XDG_RUNTIME_DIR/waybox-$sock.sock"
cleanup() { kill $(jobs -p) 2>/dev/null || true; kill $wb 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

"$build/test/touch_client" yes > /dev/null &
python3 "$here/hub2.py" &
python3 "$here/ui_gtk.py" > "$OUT/ui.log" 2>&1 &
sleep 2

key() { wtype "$@"; sleep 0.3; }
shot() { grim "$OUT/$1.png"; echo "screenshot: $OUT/$1.png"; }

echo "== a trusted agent (bound by its executable): tools, windows, a question, a form"
mkfifo "$work/calls"
AGENT_NAME="Claude" python3 "$here/agent.py" --profile trusted < "$work/calls" &
agent=$!
exec 3> "$work/calls"
echo '{"tool": "waybox.list_windows"}' >&3
sleep 1
echo '{"tool": "ui.ask", "args": {"prompt": "Which colour should the theme use?", "default": "teal"}}' >&3
sleep 1.5
shot ask
key -M ctrl -k a -m ctrl
key "orange"
key -k Return
sleep 0.5
echo '{"tool": "ui.form", "args": {"title": "New project", "schema": {"type": "object", "properties": {"name": {"type": "string", "title": "Name"}, "lang": {"type": "string", "title": "Language", "enum": ["c", "rust", "python"], "enumNames": ["C", "Rust", "Python"]}, "tests": {"type": "boolean", "title": "With tests", "default": true}, "token": {"type": "string", "title": "API token", "format": "password"}}, "required": ["name"]}}}' >&3
sleep 1.5
shot form
key "demo"
key -k Return
sleep 0.5
echo '{"tool": "ui.ask", "args": {"prompt": "Password for sudo:", "secret": true}}' >&3
sleep 1.5
shot secret
key -k Escape
sleep 0.5
echo '{"tool": "waybox.close_window", "args": {"id": "x"}}' >&3
sleep 0.5
exec 3>&-
wait $agent || true

echo
echo "== an unbound agent (its shim started from sh) asks for 'trusted': gets 'safe'"
printf '%s\n' \
	'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"Sneaky"}}}' \
	'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
	'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"waybox.focus_window","arguments":{"id":"x"}}}' \
	| sh -c "python3 '$here/shim.py' --profile trusted" \
	| python3 -c '
import json, sys
for line in sys.stdin:
    m = json.loads(line)
    r = m.get("result", {})
    if "tools" in r:
        print("tools offered:", " ".join(t["name"] for t in r["tools"]))
    elif "content" in r:
        print("focus_window:", r["content"][0]["text"])
'

echo
echo "== the audit log"
python3 -c '
import json, sys
for line in open(sys.argv[1]):
    r = json.loads(line)
    print(r["client"], r["profile"], r["tool"], r["outcome"], r.get("why", ""), json.dumps(r.get("args", {}))[:70])
' "$XDG_RUNTIME_DIR/agent-tools/audit.log"
