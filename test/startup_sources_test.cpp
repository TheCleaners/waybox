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

WB_TEST(switching_autostart_writes_hidden) {
	std::string text = "[Desktop Entry]\nName=Foo\nExec=foo\n[Desktop Action x]\nExec=y\n";
	std::string off = wb::autostart_set_enabled(text, false);
	WB_CHECK(off == "[Desktop Entry]\nName=Foo\nExec=foo\nHidden=true\n[Desktop Action x]\nExec=y\n");
	wb::AutostartEntry e;
	WB_CHECK(wb::parse_autostart(off, {}, e) && !e.enabled);
	WB_CHECK(wb::autostart_set_enabled(off, true) == text);
	std::string gnome = "[Desktop Entry]\nExec=foo\nX-GNOME-Autostart-enabled=false\n";
	wb::AutostartEntry g;
	WB_CHECK(wb::parse_autostart(wb::autostart_set_enabled(gnome, true), {}, g) && g.enabled);
	WB_CHECK(wb::autostart_set_enabled("[Desktop Entry]\nExec=foo\nHidden=false\n", false) ==
			"[Desktop Entry]\nExec=foo\nHidden=true\n");
}
