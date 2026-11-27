#include "wb_test.hpp"

#include "waybox/startup_sources.hpp"

namespace {
const char *kScript = R"(#!/bin/sh
# Panel
command -v waybar >/dev/null 2>&1 && waybar &
mako &
#swayidle -w timeout 600 'wlopm --off "*"' resume 'wlopm --on "*"' &
# To lock the screen, add for example: timeout 300 'swaylock -f'
if which swaybg >/dev/null 2>&1; then
    swaybg -c '#303030' &
fi
start_wallpaper() {
    hyprpaper &
}
FOO=1 bar &
kanshi >/dev/null 2>&1 &
echo done; nm-applet &
)";
}  // namespace

WB_TEST(script_entries_are_the_simple_start_lines) {
	std::vector<wb::ScriptEntry> e = wb::script_entries(kScript);
	std::vector<std::string> progs;
	for (const auto &x : e)
		progs.push_back(x.program + (x.enabled ? "" : "(off)"));
	/* Not: swaybg and hyprpaper (in an if / a function), FOO=1 bar (an
	 * assignment), nm-applet (after another command), the prose comment. */
	WB_CHECK((progs == std::vector<std::string>{"waybar", "mako", "swayidle(off)", "kanshi"}));
	WB_CHECK(e[0].line == 2 && e[2].line == 4);
	WB_CHECK(!e[0].function);
	/* A function the script defines, started on a line of its own. */
	std::vector<wb::ScriptEntry> f = wb::script_entries(
			"load_wallpaper() {\n    swaybg &\n}\nload_wallpaper &\n");
	WB_CHECK(f.size() == 1 && f[0].program == "load_wallpaper" && f[0].function);
}

WB_TEST(switching_a_script_line_comments_it_and_nothing_else) {
	std::string off = wb::script_set_enabled(kScript, 3, false);  /* mako */
	WB_CHECK(off.find("\n#mako &\n") != std::string::npos);
	std::string on = wb::script_set_enabled(off, 3, true);
	WB_CHECK(on == kScript);
	/* The commented example back on, then off again: as it was. */
	std::string idle = wb::script_set_enabled(kScript, 4, true);
	WB_CHECK(idle.find("\nswayidle -w timeout 600") != std::string::npos);
	WB_CHECK(wb::script_set_enabled(idle, 4, false) == kScript);
	/* Not an entry (inside the if): unchanged. */
	WB_CHECK(wb::script_set_enabled(kScript, 7, false) == kScript);
	/* No trailing newline is kept as is. */
	WB_CHECK(wb::script_set_enabled("mako &", 0, false) == "#mako &");
}

WB_TEST(autostart_entries_follow_the_spec) {
	wb::AutostartEntry e;
	WB_CHECK(wb::parse_autostart("[Desktop Entry]\nName=Foo\nExec=/usr/bin/foo --tray %U\n"
			"OnlyShowIn=GNOME;KDE;\n[Desktop Action x]\nExec=other\n", {"Waybox"}, e));
	WB_CHECK(e.name == "Foo" && e.exec == "/usr/bin/foo --tray" && e.program == "foo");
	WB_CHECK(e.enabled && !e.for_here);
	wb::AutostartEntry h;
	WB_CHECK(wb::parse_autostart("[Desktop Entry]\nExec=bar\nX-GNOME-Autostart-enabled=false\n",
			{"Waybox"}, h));
	WB_CHECK(!h.enabled && h.for_here);
	wb::AutostartEntry n;
	WB_CHECK(!wb::parse_autostart("[Desktop Entry]\nName=No exec\n", {}, n));
}

/* Switched for Waybox only: ~/.config/autostart is every desktop's. */
WB_TEST(switching_autostart_is_for_waybox_only) {
	std::string text = "[Desktop Entry]\nName=Foo\nExec=foo\n[Desktop Action x]\nExec=y\n";
	std::string off = wb::autostart_set_enabled(text, false);
	WB_CHECK(off == "[Desktop Entry]\nName=Foo\nExec=foo\nNotShowIn=Waybox;\n"
			"[Desktop Action x]\nExec=y\n");
	wb::AutostartEntry e, g;
	WB_CHECK(wb::parse_autostart(off, {"Waybox"}, e) && e.enabled && !e.for_here);
	WB_CHECK(wb::parse_autostart(off, {"GNOME"}, g) && g.enabled && g.for_here);
	WB_CHECK(wb::autostart_set_enabled(off, true) == text);
	/* Others in NotShowIn kept; Waybox added once, taken out alone. */
	std::string nm = "[Desktop Entry]\nExec=nm-applet\nNotShowIn=KDE;GNOME;\n";
	std::string nm_off = wb::autostart_set_enabled(nm, false);
	WB_CHECK(nm_off == "[Desktop Entry]\nExec=nm-applet\nNotShowIn=KDE;GNOME;Waybox;\n");
	WB_CHECK(wb::autostart_set_enabled(nm_off, false) == nm_off);
	WB_CHECK(wb::autostart_set_enabled(nm_off, true) == nm);
	/* OnlyShowIn: off takes Waybox out; on puts it in. */
	WB_CHECK(wb::autostart_set_enabled("[Desktop Entry]\nExec=a\nOnlyShowIn=XFCE;\n", true) ==
			"[Desktop Entry]\nExec=a\nOnlyShowIn=XFCE;Waybox;\n");
	/* On drops what turns it off everywhere (an older wbconf's Hidden). */
	std::string gnome = "[Desktop Entry]\nExec=foo\nX-GNOME-Autostart-enabled=false\nHidden=true\n";
	wb::AutostartEntry h;
	WB_CHECK(wb::parse_autostart(wb::autostart_set_enabled(gnome, true), {"Waybox"}, h) &&
			h.enabled && h.for_here);
	/* Another desktop's name, given. */
	WB_CHECK(wb::autostart_set_enabled("[Desktop Entry]\nExec=b\n", false, "sway") ==
			"[Desktop Entry]\nExec=b\nNotShowIn=sway;\n");
}

WB_TEST(script_starters_direct_function_and_elsewhere) {
	const char *script = R"SH(#!/bin/sh
mako &
#swaybg -c '#000' &
pick() {
    ls ~/wallpapers | shuf -n 1
}
load_wallpaper() {
    if which hyprpaper >/dev/null 2>&1; then
        hyprpaper &
    elif which swaybg >/dev/null 2>&1; then
        swaybg -m fill -i "$(pick)" &
    fi
}
load_wallpaper &
if true; then
    waybar &
fi
command -v kanshi >/dev/null 2>&1 && kanshi &
)SH";
	/* Through a function: the line calling it. */
	wb::ScriptStarters s = wb::script_starters(script, "swaybg");
	WB_CHECK(s.lines.size() == 1);
	WB_CHECK(s.lines[0].line == 13 && s.lines[0].program == "load_wallpaper");
	WB_CHECK(s.functions.size() == 1 && s.functions[0] == "load_wallpaper");
	WB_CHECK(!s.elsewhere);
	/* Its own line. */
	s = wb::script_starters(script, "mako");
	WB_CHECK(s.lines.size() == 1 && s.lines[0].line == 1 && s.functions.empty());
	WB_CHECK(!s.elsewhere);
	s = wb::script_starters(script, "kanshi");
	WB_CHECK(s.lines.size() == 1 && s.lines[0].line == 17);
	/* In an if block: no line to switch. */
	s = wb::script_starters(script, "waybar");
	WB_CHECK(s.lines.empty() && s.elsewhere);
	/* Not started (or only in a comment). */
	s = wb::script_starters(script, "swayidle");
	WB_CHECK(s.lines.empty() && !s.elsewhere);
	/* A function that runs it, called inside an if block: elsewhere. */
	s = wb::script_starters("#!/bin/sh\nf() {\n  wlsunset &\n}\nif true; then f; fi\n",
			"wlsunset");
	WB_CHECK(s.lines.empty() && s.elsewhere);
}

WB_TEST(script_start_command_strips) {
	WB_CHECK(wb::script_start_command("mako &") == "mako");
	WB_CHECK(wb::script_start_command("  swaybg -c '#000' -m fill -i ~/a.jpg &  ") ==
			"swaybg -c '#000' -m fill -i ~/a.jpg");
	WB_CHECK(wb::script_start_command("command -v kanshi >/dev/null 2>&1 && kanshi &") ==
			"kanshi");
	WB_CHECK(wb::script_start_command("which x && x --opt 'a && b' &") == "x --opt 'a && b'");
	WB_CHECK(wb::script_start_command("foo") == "foo");
}

WB_TEST(script_lines_that_are_not_start_lines) {
	/* Lines that look like "prog &" but aren't commands of their own:
	 * switching them off would corrupt the script. */
	const char *script = R"SH(#!/bin/sh
cat > ~/.config/thing.conf <<EOF
swaybg &
EOF
cat <<-'END'
	mako &
	END
long_command --option \
    waybar &
MSG="first line
kanshi &
last line"
wlsunset &
)SH";
	std::vector<wb::ScriptEntry> entries = wb::script_entries(script);
	/* Only wlsunset is a start line of its own. */
	WB_CHECK(entries.size() == 1);
	WB_CHECK(!entries.empty() && entries[0].program == "wlsunset" && entries[0].line == 12);
	/* None of the others counts as started at all. */
	for (const char *p : {"swaybg", "mako", "kanshi"}) {
		wb::ScriptStarters s = wb::script_starters(script, p);
		WB_CHECK(s.lines.empty() && !s.elsewhere);
	}
	/* waybar is really started, by a continued command: elsewhere, not a
	 * line to switch. */
	wb::ScriptStarters w = wb::script_starters(script, "waybar");
	WB_CHECK(w.lines.empty() && w.elsewhere);
	/* Switching a here-doc, continuation or string line is refused (the
	 * script comes back as it was). */
	WB_CHECK(wb::script_set_enabled(script, 2, false) == script);
	WB_CHECK(wb::script_set_enabled(script, 8, false) == script);
	WB_CHECK(wb::script_set_enabled(script, 10, false) == script);
}

/* "(waybar || yambar) &": a start line for waybar, yambar its fallback;
 * found for either (so taking either over offers to switch it off). */
WB_TEST(script_entries_alternatives) {
	const char *script = R"SH(#!/bin/sh
(waybar || yambar) &
mako -c x || dunst &
(swaybg -i a.png; waybar) &
(foo || bar && baz) &
)SH";
	std::vector<wb::ScriptEntry> entries = wb::script_entries(script);
	WB_CHECK(entries.size() == 2);
	if (entries.size() == 2) {
		WB_CHECK(entries[0].program == "waybar" && entries[0].line == 1 &&
				(entries[0].alternatives == std::vector<std::string>{"yambar"}));
		WB_CHECK(entries[1].program == "mako" &&
				(entries[1].alternatives == std::vector<std::string>{"dunst"}));
	}
	for (const char *p : {"waybar", "yambar"}) {
		wb::ScriptStarters s = wb::script_starters(script, p);
		WB_CHECK(s.lines.size() == 1 && s.lines[0].line == 1);
	}
	/* Switched off as a whole line. */
	std::string off = wb::script_set_enabled(script, 1, false);
	WB_CHECK(off.find("#(waybar || yambar) &") != std::string::npos);
}

WB_TEST(script_starters_mentions_are_runs) {
	/* A function that only names the program (a message, a check, a kill)
	 * doesn't start it. */
	const char *script = R"SH(#!/bin/sh
announce() {
    notify-send "swaybg started"
    pkill swaybg
    command -v swaybg >/dev/null
}
announce &
start() {
    command -v swaybg >/dev/null && swaybg -c '#000' &
}
start &
)SH";
	wb::ScriptStarters s = wb::script_starters(script, "swaybg");
	WB_CHECK(s.lines.size() == 1);
	WB_CHECK(!s.lines.empty() && s.lines[0].program == "start");
	WB_CHECK(!s.elsewhere);
}

WB_TEST(find_script_entry_after_changes) {
	auto was = wb::script_entries("#!/bin/sh\nmako &\nwaybar &\n");
	WB_CHECK(wb::find_script_entry(was, "mako &", 1) == 1);
	/* A line put above: found where it went. */
	auto now = wb::script_entries("#!/bin/sh\nkanshi &\nmako &\nwaybar &\n");
	WB_CHECK(wb::find_script_entry(now, "mako &", 1) == 2);
	/* Commented out since: still the entry. */
	auto off = wb::script_entries("#!/bin/sh\n#mako &\nwaybar &\n");
	WB_CHECK(wb::find_script_entry(off, "mako &", 1) == 1);
	/* Gone. */
	WB_CHECK(wb::find_script_entry(now, "dunst &", 1) == -1);
	/* Twice: the one nearest where it was. */
	auto twice = wb::script_entries("#!/bin/sh\nfoo &\nbar &\nbaz &\nfoo &\n");
	WB_CHECK(wb::find_script_entry(twice, "foo &", 1) == 1);
	WB_CHECK(wb::find_script_entry(twice, "foo &", 5) == 4);
}

