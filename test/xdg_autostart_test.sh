#!/bin/sh
# Test data/xdg-autostart against fixture autostart directories. Each entry's
# Exec creates a marker file; the test checks which markers exist.
# Usage: xdg_autostart_test.sh <path-to-xdg-autostart>
script=$1
t=$(mktemp -d) || exit 1
trap 'rm -rf -- "$t"' EXIT
mkdir -p "$t/home/autostart" "$t/sys/autostart" "$t/out" "$t/bin" "$t/cwd"
# Run from a folder with a file named like the desktop: a pattern in a
# .desktop value must not expand to it.
: > "$t/cwd/Waybox"
out=$t/out

entry() {  # entry <dir> <name> <extra lines...>
	dir=$1 name=$2
	shift 2
	{
		echo "[Desktop Entry]"
		echo "Type=Application"
		echo "Name=$name"
		echo "Exec=touch $out/$name"
		for line in "$@"; do echo "$line"; done
	} > "$t/$dir/autostart/$name.desktop"
}

entry sys plain
entry sys only-gnome "OnlyShowIn=GNOME;KDE;"
entry sys only-waybox "OnlyShowIn=GNOME;Waybox;"
entry sys not-waybox "NotShowIn=Waybox;"
entry sys not-other "NotShowIn=KDE;GNOME;"
entry sys hidden "Hidden=true"
entry sys gnome-disabled "X-GNOME-Autostart-enabled=false"
entry sys tryexec-missing "TryExec=/nonexistent/program"
entry sys tryexec-found "TryExec=sh"
entry sys cond-gsettings-true "AutostartCondition=GSettings org.test enabled-key"
entry sys cond-gsettings-false "AutostartCondition=GSettings org.test disabled-key"
entry sys cond-unless-exists "AutostartCondition=unless-exists nothing-here"
entry sys cond-if-exists "AutostartCondition=if-exists nothing-here"
entry sys cond-unknown "AutostartCondition=GNOME3 if-session gnome"
entry sys only-pattern "OnlyShowIn=W*;"
entry sys geoclue-demo-agent
# A user file name with a space must not hide a system one it ends with.
printf '[Desktop Entry]\nType=Application\nExec=touch %s/spaced\n' "$out" \
	> "$t/home/autostart/a b.desktop"
printf '[Desktop Entry]\nType=Application\nExec=touch %s/b-sys\n' "$out" \
	> "$t/sys/autostart/b.desktop"
# Shadowing: the user copy (Hidden) wins over the system copy.
entry sys shadowed
entry home shadowed "Hidden=true"
# Only [Desktop Entry] keys count; field codes are removed.
cat > "$t/sys/autostart/actions.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=actions
Exec=touch $out/actions %U
Actions=new;

[Desktop Action new]
Name=New
Exec=touch $out/action-ran
EOF
# A fake gsettings for the GSettings conditions.
cat > "$t/bin/gsettings" <<'EOF'
#!/bin/sh
[ "$3" = "enabled-key" ] && echo true || echo false
EOF
chmod +x "$t/bin/gsettings"

run() {
	rm -f "$out"/*
	(cd "$t/cwd" && env XDG_CONFIG_HOME="$t/home" XDG_CONFIG_DIRS="$t/sys" \
		PATH="$t/bin:$PATH" XDG_CURRENT_DESKTOP=Waybox "$@" sh "$script")
	sleep 0.5  # entries are started in the background
}

fail=0
expect() {  # expect <yes|no> <marker> <why>
	if [ -e "$out/$2" ]; then got=yes; else got=no; fi
	if [ "$got" != "$1" ]; then
		echo "FAIL: $2 started=$got, expected $1 ($3)"
		fail=1
	fi
}

run env -u WB_AUTOSTART_ENVIRONMENT
expect yes plain "no OnlyShowIn/NotShowIn"
expect no only-gnome "OnlyShowIn lists other desktops"
expect yes only-waybox "OnlyShowIn lists Waybox"
expect no not-waybox "NotShowIn lists Waybox"
expect yes not-other "NotShowIn lists only other desktops"
expect no hidden "Hidden=true"
expect no gnome-disabled "X-GNOME-Autostart-enabled=false"
expect no tryexec-missing "TryExec not found"
expect yes tryexec-found "TryExec found"
expect yes cond-gsettings-true "GSettings condition true"
expect no cond-gsettings-false "GSettings condition false"
expect yes cond-unless-exists "unless-exists, file absent"
expect no cond-if-exists "if-exists, file absent"
expect no cond-unknown "unknown condition"
expect no shadowed "user Hidden copy shadows the system one"
expect yes actions "main Exec runs, %U removed"
expect no action-ran "[Desktop Action] Exec must not run"
expect no "actions %U" "field code must not reach the command"
expect no only-pattern "a * in OnlyShowIn isn't a file pattern"
expect yes spaced "a file name with a space"
expect yes b-sys "not hidden by a name ending the same after a space"

run env WB_AUTOSTART_ENVIRONMENT=all
expect yes only-gnome "WB_AUTOSTART_ENVIRONMENT=all ignores OnlyShowIn"
expect no hidden "Hidden still applies with all"

run env WB_AUTOSTART_ENVIRONMENT=Waybox:KDE
expect yes only-gnome "OnlyShowIn=GNOME;KDE; with KDE listed"
expect no not-other "NotShowIn=KDE;GNOME; with KDE listed"

# GeoClue's demo agent (yes to every app, unasked): not started where
# Waybox's location agent is installed (it asks), else as any entry. (A
# PATH of its own: the real agent may be installed where the test runs.)
mkdir -p "$t/agentbin"
printf '#!/bin/sh\n' > "$t/agentbin/waybox-location-agent"
chmod +x "$t/agentbin/waybox-location-agent"
run env PATH="$t/bin:/usr/bin:/bin"
expect yes geoclue-demo-agent "no location agent installed: the demo agent starts"
run env PATH="$t/agentbin:$t/bin:/usr/bin:/bin"
expect no geoclue-demo-agent "Waybox's location agent installed: the demo agent doesn't start"

[ "$fail" = 0 ] && echo "xdg-autostart: OK"
exit "$fail"
