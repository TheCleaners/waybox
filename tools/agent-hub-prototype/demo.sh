#!/bin/sh
# Exercises the prototype on a private session bus (never the user's):
# a hub, a consent backend that answers as told, and a Waybar-style surface.
# Usage: demo.sh   (run inside dbus-run-session; it re-execs itself if not)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "${AGENT_DEMO_BUS:-}" ]; then
	AGENT_DEMO_BUS=1 exec dbus-run-session -- "$0" "$@"
fi
call() { gdbus call --session -d org.waybox.AgentHub1 -o /org/waybox/AgentHub1 -m "$@"; }
step() { printf '\n== %s\n' "$*"; sleep 0.3; }

python3 "$here/hub.py" & hub=$!
python3 "$here/surface.py" | sed -u 's/^/surface: /' & surf=$!
sleep 1

step "an agent connects (profile 'cloud', client says 'Claude')"
agent=$(call org.waybox.AgentHub1.Debug.Connect cloud Claude | tr -d "(',)")

step "a read call: allowed by the level, no question"
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox list_windows read

step "a new provider appears: pending, so its tools are refused"
call org.waybox.AgentHub1.Debug.AddProvider org.waybar.Waybar Waybar act
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybar.Waybar media_play_pause act
step "a surface tries to approve it over D-Bus: not possible (no such method)"
call org.waybox.AgentHub1.ApproveProvider org.waybar.Waybar 2>&1 | head -1 || true
step "the user approves it in the compositor's confirmation (debug stand-in)"
call org.waybox.AgentHub1.Debug.Approve org.waybar.Waybar
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybar.Waybar media_play_pause act

step "an exec call, level 'ask', and no consent backend yet: denied"
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox run_command exec

step "the consent backend comes up; the user grants for this session"
ANSWER=grant:session python3 "$here/consent.py" & consent=$!
sleep 1
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox run_command exec
step "the same call again: covered by the grant, no question"
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox run_command exec
gdbus introspect --session -d org.waybox.AgentHub1 -o /org/waybox/AgentHub1 -p \
	| grep -A1 "Grants" | head -2

step "input: level 'off' in this preset; refused without asking"
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox type_text input

step "the stop key (or a panel's stop button): grants gone"
python3 "$here/surface.py" --stop
call org.waybox.AgentHub1.Debug.Call "$agent" org.waybox.Waybox run_command exec || true

step "the audit's last calls"
call org.waybox.AgentHub1.RecentCalls 3 | tr '{' '\n' | grep -o "'tool': <'[a-z_]*'>, 'outcome': <'[a-z]*'>\|'outcome': <'[a-z]*'>" | head -5

kill $consent $surf $hub 2>/dev/null || true
