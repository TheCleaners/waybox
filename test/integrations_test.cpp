#include "wb_test.hpp"

#include "waybox/integrations.hpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using wb::Integration;

namespace {
const char *kWlsunset = R"(# night light
[Integration]
Name=wlsunset
Category=NightLight
Comment=Warmer colours at night
Exec=wlsunset -l 51.5 -L -0.1

[Option latitude]
Label=Latitude
Flag=-l
Hint=51.5

[Option longitude]
Label=Longitude
Flag=-L

[Option broken]
Label=No flag
)";
}  // namespace

WB_TEST(integration_parses_with_options) {
	auto in = wb::parse_integration("wlsunset", kWlsunset);
	WB_CHECK(in.has_value());
	WB_CHECK(in->name == "wlsunset" && in->category == "NightLight");
	WB_CHECK(in->binary == "wlsunset");  /* from Exec */
	WB_CHECK(in->options.size() == 2);   /* the one without a flag dropped */
	WB_CHECK(in->options[0].id == "latitude" && in->options[0].flag == "-l");
	WB_CHECK(in->options[0].hint == "51.5");
	WB_CHECK(!wb::parse_integration("x", "[Integration]\nName=x\n"));  /* no Exec */
}

WB_TEST(command_words_round_trip) {
	auto w = wb::split_command(R"(swaybg -i "/home/me/My Pictures/a b.png" -c '#333' x\ y)");
	WB_CHECK(w.size() == 6);
	WB_CHECK(w[2] == "/home/me/My Pictures/a b.png");
	WB_CHECK(w[4] == "#333" && w[5] == "x y");
	WB_CHECK(wb::split_command(wb::join_command(w)) == w);
	WB_CHECK(wb::join_command({"a", "it's", ""}) == "a 'it'\\''s' ''");
}

WB_TEST(options_read_and_written_in_place) {
	auto in = *wb::parse_integration("wlsunset", kWlsunset);
	auto v = wb::read_options(in, "wlsunset -t 3500 -l 48.8 -L 2.3");
	WB_CHECK(v["latitude"] == "48.8" && v["longitude"] == "2.3");
	/* changed, dropped, unknown flags (-t) kept in place */
	WB_CHECK(wb::write_options(in, "wlsunset -t 3500 -l 48.8 -L 2.3",
			{{"latitude", "40"}, {"longitude", ""}}) == "wlsunset -t 3500 -l 40");
	/* added at the end; from Exec when there's no command yet */
	WB_CHECK(wb::write_options(in, "wlsunset", {{"longitude", "-3.7"}}) ==
			"wlsunset -L -3.7");
	WB_CHECK(wb::write_options(in, "", {{"latitude", "1"}, {"longitude", "2"}}) ==
			"wlsunset -l 1 -L 2");
}

WB_TEST(startup_programs_match_integrations) {
	std::vector<Integration> all = {*wb::parse_integration("wlsunset", kWlsunset),
			*wb::parse_integration("waybar", "[Integration]\nName=Waybar\nExec=waybar\n")};
	WB_CHECK(wb::match_integration(all, "night", "/usr/bin/wlsunset -l 1") == &all[0]);
	WB_CHECK(wb::match_integration(all, "waybar", "something else") == &all[1]);
	WB_CHECK(wb::match_integration(all, "mine", "my-script") == nullptr);
}

WB_TEST(programs_found_in_autostart_script) {
	const char *script = R"(#!/bin/sh
# mako is commented out here
dbus-update-activation-environment --all
(waybar || yambar) &
  /usr/bin/swaybg -i x.png & # wallpaper
echo mako-not-really
)";
	auto found = wb::programs_in_script(script,
			{"mako", "waybar", "yambar", "swaybg", "wlsunset"});
	WB_CHECK(found == (std::vector<std::string>{"swaybg", "waybar", "yambar"}));
}

/* A function's programs count only if it's called: one whose call is
 * commented out (as wbconf's takeover leaves it) doesn't start them. */
WB_TEST(programs_in_script_functions) {
	const char *script = R"(#!/bin/sh
load_wallpaper() {
    if which swaybg >/dev/null 2>&1; then
        swaybg -c '#303030' &
    fi
}

start_panel()
{
    waybar &
}

function notify { mako & }

helper() { start_panel; }

#load_wallpaper &
helper
)";
	auto found = wb::programs_in_script(script, {"swaybg", "waybar", "mako"});
	WB_CHECK(found == (std::vector<std::string>{"waybar"}));
	/* Called again: it's back. */
	std::string again = std::string(script) + "load_wallpaper &\nnotify\n";
	found = wb::programs_in_script(again, {"swaybg", "waybar", "mako"});
	WB_CHECK(found == (std::vector<std::string>{"mako", "swaybg", "waybar"}));
}

WB_TEST(integrations_load_user_first_sorted) {
	char tmpl[] = "/tmp/wb-integ-XXXXXX";
	std::filesystem::path root = mkdtemp(tmpl);
	auto write = [](const std::filesystem::path &p, const char *text) {
		std::filesystem::create_directories(p.parent_path());
		std::ofstream(p) << text;
	};
	write(root / "user" / "waybar.integration",
			"[Integration]\nName=Waybar (mine)\nCategory=Panel\nExec=waybar -c x\n");
	write(root / "sys" / "waybar.integration",
			"[Integration]\nName=Waybar\nCategory=Panel\nExec=waybar\n");
	write(root / "sys" / "mako.integration",
			"[Integration]\nName=mako\nCategory=Notifications\nExec=mako\n");
	write(root / "sys" / "notes.txt", "ignored");
	auto all = wb::load_integrations({(root / "user").string(), (root / "sys").string(),
			(root / "missing").string()});
	WB_CHECK(all.size() == 2);
	WB_CHECK(all[0].name == "Waybar (mine)");  /* Panel before Notifications */
	WB_CHECK(all[1].id == "mako");
	std::filesystem::remove_all(root);
	auto dirs = wb::integration_dirs("/h", "", "/a:/b", "/inst");
	WB_CHECK(dirs.size() == 4 && dirs[0] == "/h/.config/waybox/integrations" &&
			dirs[2] == "/b/waybox/integrations" && dirs[3] == "/inst");
}

WB_TEST(integration_binding_kind_and_key) {
	auto in = wb::parse_integration("bemoji",
			"[Integration]\nName=bemoji\nCategory=Pickers\nExec=bemoji -t -n\n"
			"Kind=binding\nKey=W-period\n");
	WB_CHECK(in && in->binding && in->key == "W-period");
	WB_CHECK(!wb::parse_integration("mako", "[Integration]\nName=mako\nExec=mako\n")->binding);
}

WB_TEST(integration_input_method_kind_and_env) {
	auto in = wb::parse_integration("fcitx5",
			"[Integration]\nName=Fcitx 5\nCategory=InputMethod\nExec=fcitx5 --replace\n"
			"Kind=inputmethod\nEnv=XMODIFIERS=@im=fcitx\nEnv=QT_IM_MODULE=fcitx\nEnv=bad\n");
	WB_CHECK(in && in->input_method && !in->binding);
	WB_CHECK(in->env.size() == 2);
	WB_CHECK(in->env[0].first == "XMODIFIERS" && in->env[0].second == "@im=fcitx");
}

WB_TEST(options_keep_the_rest_of_the_command_as_written) {
	auto in = *wb::parse_integration("swaybg",
			"[Integration]\nName=swaybg\nExec=swaybg -c '#303030'\n"
			"[Option image]\nFlag=-i\n[Option mode]\nFlag=-m\n");
	/* ~ and $VAR stay expandable; untouched words keep their quoting. */
	WB_CHECK(wb::write_options(in, "swaybg -c '#303030'", {{"image", "~/Pictures/bg.jpg"}}) ==
			"swaybg -c '#303030' -i ~/Pictures/bg.jpg");
	WB_CHECK(wb::write_options(in, "swaybg -i $HOME/a.png -c '#303030'",
			{{"image", "$HOME/a.png"}, {"mode", "fill"}}) ==
			"swaybg -i $HOME/a.png -c '#303030' -m fill");
	/* A value with spaces is quoted; a dropped option leaves no gap. */
	WB_CHECK(wb::write_options(in, "swaybg -i x.png -m fit", {{"image", "My Pictures/b.png"}}) ==
			"swaybg -i 'My Pictures/b.png'");
}

WB_TEST(program_status_says_where_a_program_comes_from) {
	/* Off in wbconf, started by the autostart script: say so, no warning. */
	wb::ProgramStatus st = wb::program_status(true, true, true, false);
	WB_CHECK(st.running && st.from_script && !st.twice);
	/* On as well: it would run twice. */
	st = wb::program_status(true, true, true, true);
	WB_CHECK(st.twice && !st.from_script);
	/* Only wbconf's: nothing about the script. */
	st = wb::program_status(true, false, false, true);
	WB_CHECK(!st.running && !st.twice && !st.from_script);
	st = wb::program_status(false, false, false, false);
	WB_CHECK(st.not_installed && !st.running);
}

WB_TEST(version_from_real_outputs) {
	/* As the programs print it. */
	WB_CHECK(wb::version_from_output("swaybg version 1.2.2\n") == "1.2.2");
	WB_CHECK(wb::version_from_output("Waybar v0.15.0\n") == "0.15.0");
	WB_CHECK(wb::version_from_output("ibus-daemon - Version 1.5.35\n") == "1.5.35");
	WB_CHECK(wb::version_from_output("Dunst - A customizable and lightweight notification-daemon "
			"1.13.2 (2026-03-18)\n") == "1.13.2");
	WB_CHECK(wb::version_from_output("\"No Flameshot translation found for C\"\n"
			"Flameshot v14.0.0 (Debian-14.0.0-4)\nCompiled with Qt 6.10.2\n") == "14.0.0");
	WB_CHECK(wb::version_from_output("version\t0.7.0\n") == "0.7.0");
	WB_CHECK(wb::version_from_output("gammastep 2.0.11\n") == "2.0.11");
	WB_CHECK(wb::version_from_output("usage: foo [-h]\n").empty());
	WB_CHECK(wb::version_from_output("").empty());
}

WB_TEST(parse_version_command) {
	auto in = wb::parse_integration("swaybg", "[Integration]\nName=swaybg\nExec=swaybg\n"
			"Version=swaybg -v\n");
	WB_CHECK(in && in->version_command == "swaybg -v");
	auto none = wb::parse_integration("mako", "[Integration]\nName=mako\nExec=mako\n");
	WB_CHECK(none && none->version_command.empty());
}

WB_TEST(option_types_parse) {
	auto in = wb::parse_integration("bg", R"([Integration]
Name=bg
Exec=bg -c '#303030'
[Option image]
Flag=-i
Type=image
[Option mode]
Flag=-m
Type=choice
Choices= fill, fit ,fill,, center
[Option every]
Flag=-e
Type=number
Min=1
Max=1440
[Option bad-range]
Flag=-r
Type=number
Min=10
Max=1
[Option empty-choice]
Flag=-x
Type=choice
[Option quiet]
Flag=-q
Type=switch
[Option what]
Flag=-w
Type=sparkles
[Option colour]
Flag=-c
Type=colour
)");
	WB_CHECK(in.has_value());
	const auto &o = in->options;
	WB_CHECK(o.size() == 8);
	WB_CHECK(o[0].type == wb::OptionType::Image);
	WB_CHECK(o[1].type == wb::OptionType::Choice);
	WB_CHECK((o[1].choices == std::vector<std::string>{"fill", "fit", "center"}));
	WB_CHECK(o[2].type == wb::OptionType::Number && o[2].min == 1.0 && o[2].max == 1440.0);
	WB_CHECK(!o[3].min && !o[3].max);              /* min above max: dropped */
	WB_CHECK(o[4].type == wb::OptionType::Text);   /* a choice of nothing */
	WB_CHECK(o[5].type == wb::OptionType::Switch);
	WB_CHECK(o[6].type == wb::OptionType::Text);   /* unknown type */
	WB_CHECK(o[7].type == wb::OptionType::Color);  /* either spelling */
	WB_CHECK(wb::option_type_from_name("image-or-folder") == wb::OptionType::ImageOrFolder);
	WB_CHECK(wb::option_type_from_name("image-folder") == wb::OptionType::ImageFolder);
	WB_CHECK(wb::option_type_from_name("folder") == wb::OptionType::Folder);
	WB_CHECK(wb::option_type_from_name("file") == wb::OptionType::File);

	WB_CHECK(wb::option_value_valid(o[2], ""));
	WB_CHECK(wb::option_value_valid(o[2], "60"));
	WB_CHECK(wb::option_value_valid(o[2], "1") && wb::option_value_valid(o[2], "1440"));
	WB_CHECK(!wb::option_value_valid(o[2], "0") && !wb::option_value_valid(o[2], "1441"));
	WB_CHECK(!wb::option_value_valid(o[2], "6O") && !wb::option_value_valid(o[2], "0x10"));
	WB_CHECK(!wb::option_value_valid(o[2], "nan") && !wb::option_value_valid(o[2], "inf"));
	WB_CHECK(wb::option_value_valid(o[1], "fit") && !wb::option_value_valid(o[1], "tile"));
	WB_CHECK(wb::option_value_valid(o[5], "on") && !wb::option_value_valid(o[5], "yes"));
	WB_CHECK(wb::option_value_valid(o[0], "$HOME/x.png"));  /* paths: anything */
}

WB_TEST(switch_options_read_and_written) {
	auto in = wb::parse_integration("p", R"([Integration]
Name=p
Exec=prog --tray
[Option quiet]
Flag=-q
Type=switch
[Option size]
Flag=-s
)");
	WB_CHECK(in.has_value());
	WB_CHECK(wb::read_options(*in, "prog --tray").count("quiet") == 0);
	auto v = wb::read_options(*in, "prog -q --tray");
	WB_CHECK(v["quiet"] == "on");
	/* At the end, with no word after it: still there. */
	WB_CHECK(wb::read_options(*in, "prog --tray -q")["quiet"] == "on");
	/* A switch's flag isn't another option's value. */
	auto s = wb::read_options(*in, "prog -s 12 -q");
	WB_CHECK(s["size"] == "12" && s["quiet"] == "on");
	WB_CHECK(wb::write_options(*in, "prog --tray", {{"quiet", "on"}}) == "prog --tray -q");
	WB_CHECK(wb::write_options(*in, "prog -q --tray", {{"quiet", "on"}}) == "prog -q --tray");
	WB_CHECK(wb::write_options(*in, "prog -q --tray", {}) == "prog --tray");
	WB_CHECK(wb::write_options(*in, "prog --tray -q", {{"size", "3"}}) == "prog --tray -s 3");
	WB_CHECK(wb::write_options(*in, "prog --tray -q", {{"quiet", "on"}, {"size", "3"}}) ==
			"prog --tray -q -s 3");
}

WB_TEST(integration_configure_tool) {
	auto in = wb::parse_integration("waypaper", "[Integration]\nName=Waypaper\n"
			"Exec=waypaper --restore\nBinary=waypaper\nConfigure=waypaper --folder ~/Pictures\n");
	WB_CHECK(in.has_value() && in->configure == "waypaper --folder ~/Pictures");
	auto none = wb::parse_integration("x", "[Integration]\nName=x\nExec=x\n");
	WB_CHECK(none.has_value() && none->configure.empty());
}

WB_TEST(integration_page_and_defaults) {
	auto in = wb::parse_integration("swaybg", "[Integration]\nName=swaybg\nExec=swaybg\n"
			"Page=wallpaper\n[Option mode]\nFlag=-m\nType=choice\nChoices=fill,stretch\n"
			"Default=stretch\n");
	WB_CHECK(in.has_value() && in->page == "wallpaper");
	WB_CHECK(in->options.size() == 1 && in->options[0].default_value == "stretch");
	/* A default is shown, never written: the command stays without it. */
	WB_CHECK(wb::write_options(*in, "swaybg", {}) == "swaybg");
}

namespace {

bool has_problem(const wb::IntegrationCheck &c, int line, bool error, const char *part) {
	for (const wb::IntegrationProblem &p : c.problems)
		if (p.line == line && p.error == error && p.message.find(part) != std::string::npos)
			return true;
	return false;
}

}  // namespace

WB_TEST(check_a_clean_file) {
	/* The sample's option without a flag: its one warning. */
	auto c = wb::check_integration("wlsunset", kWlsunset);
	WB_CHECK(c.integration.has_value() && c.problems.size() == 1 &&
			has_problem(c, 17, false, "[Option broken] has no Flag="));
	/* FormatVersion 1 or 1.x, or none: read. */
	for (const char *v : {"1", "1.0", "1.7"}) {
		auto f = wb::check_integration("x", std::string("[Integration]\nFormatVersion=") + v +
				"\nName=x\nExec=x\n");
		WB_CHECK(f.integration && f.problems.empty());
	}
}

WB_TEST(check_says_what_is_wrong_and_where) {
	const char *text =
		"Stray=1\n"                          /* 1: before any section */
		"[Integration]\n"                    /* 2 */
		"Name=demo\n"                        /* 3 */
		"Exec=demo -q\n"                     /* 4 */
		"Colour=red\n"                       /* 5: unknown key */
		"Kind=sometimes\n"                   /* 6: unknown kind */
		"Env=NOEQUALS\n"                     /* 7 */
		"Name=demo again\n"                  /* 8: given again */
		"not a key line\n"                   /* 9 */
		"[Option quiet]\n"                   /* 10: no Flag */
		"Label=Quiet\n"                      /* 11 */
		"[Option every]\n"                   /* 12 */
		"Flag=-e\n"                          /* 13 */
		"Type=number\n"                      /* 14 */
		"Min=10\n"                           /* 15 */
		"Max=1\n"                            /* 16: below Min */
		"[Option mode]\n"                    /* 17 */
		"Flag=-m\n"                          /* 18 */
		"Type=choice\n"                      /* 19: no Choices */
		"[Option size]\n"                    /* 20 */
		"Flag=-s\n"                          /* 21 */
		"Type=huge\n"                        /* 22: unknown type */
		"Min=x\n"                            /* 23: not a number */
		"[Other]\n"                          /* 24: unknown section */
		"Key=ignored\n";                     /* 25: in it: not said again */
	auto c = wb::check_integration("demo", text);
	WB_CHECK(c.integration.has_value());  /* warnings only: still usable */
	WB_CHECK(has_problem(c, 1, false, "before any section"));
	WB_CHECK(has_problem(c, 5, false, "unknown key Colour="));
	WB_CHECK(has_problem(c, 6, false, "Kind=sometimes"));
	WB_CHECK(has_problem(c, 7, false, "not NAME=VALUE"));
	WB_CHECK(has_problem(c, 8, false, "given again"));
	WB_CHECK(has_problem(c, 9, false, "not a Key=value line"));
	WB_CHECK(has_problem(c, 10, false, "[Option quiet] has no Flag="));
	WB_CHECK(has_problem(c, 12, false, "Min= is above Max="));
	WB_CHECK(has_problem(c, 17, false, "Type=choice without Choices="));
	WB_CHECK(has_problem(c, 22, false, "Type=huge"));
	WB_CHECK(has_problem(c, 23, false, "Min=x"));
	WB_CHECK(has_problem(c, 24, false, "unknown section [Other]"));
	WB_CHECK(!has_problem(c, 25, false, ""));
	/* What's read is what the warnings say. */
	const Integration &in = *c.integration;
	WB_CHECK(in.name == "demo again" && in.options.size() == 3);
	WB_CHECK(in.options[0].id == "every" && !in.options[0].min && !in.options[0].max);
	WB_CHECK(in.options[1].type == wb::OptionType::Text);
	/* Sorted by line. */
	for (size_t i = 1; i < c.problems.size(); ++i)
		WB_CHECK(c.problems[i - 1].line <= c.problems[i].line);
}

WB_TEST(check_errors_make_it_unusable) {
	auto none = wb::check_integration("x", "Name=x\n");
	WB_CHECK(!none.integration && has_problem(none, 0, true, "no [Integration]"));
	auto no_exec = wb::check_integration("x", "[Integration]\nName=x\n");
	WB_CHECK(!no_exec.integration && has_problem(no_exec, 0, true, "no Exec="));
	auto no_name = wb::check_integration("x", "[Integration]\nExec=x\n");
	WB_CHECK(!no_name.integration && has_problem(no_name, 0, true, "no Name="));
	auto blank = wb::check_integration("x", "[Integration]\nName=x\nExec=\"\" ''\n");
	WB_CHECK(!blank.integration);
	/* A newer major version: skipped, not misread. */
	auto newer = wb::check_integration("x", "[Integration]\nFormatVersion=2\nName=x\nExec=x\n");
	WB_CHECK(!newer.integration && has_problem(newer, 2, true, "newer"));
	WB_CHECK(!wb::parse_integration("x", "[Integration]\nFormatVersion=2.1\nName=x\nExec=x\n"));
	auto odd = wb::check_integration("x", "[Integration]\nFormatVersion=one\nName=x\nExec=x\n");
	WB_CHECK(odd.integration && has_problem(odd, 2, false, "not a version"));
	/* An id with characters a file name shouldn't have. */
	auto id = wb::check_integration("my tool", "[Integration]\nName=x\nExec=x\n");
	WB_CHECK(id.integration && has_problem(id, 0, false, "the file's name"));
	/* A binding without a key; a switch's Default. */
	auto b = wb::check_integration("x", "[Integration]\nName=x\nExec=x\nKind=binding\n"
			"[Option on]\nFlag=-o\nType=switch\nDefault=yes\n");
	WB_CHECK(b.integration && has_problem(b, 0, false, "without a Key=") &&
			has_problem(b, 5, false, "is a switch"));
}

WB_TEST(shipped_integrations_are_clean) {
	/* Every file Waybox ships reads without a single warning. */
	const char *dir = getenv("WB_INTEGRATIONS_SRC");
	if (dir == nullptr)
		return;  /* run from meson, which sets it */
	int files = 0;
	for (const auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".integration")
			continue;
		++files;
		std::ifstream f(e.path());
		std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		auto c = wb::check_integration(e.path().stem().string(), text);
		WB_CHECK(c.integration.has_value());
		for (const wb::IntegrationProblem &p : c.problems)
			fprintf(stderr, "%s:%d: %s\n", e.path().c_str(), p.line, p.message.c_str());
		WB_CHECK(c.problems.empty());
	}
	WB_CHECK(files > 20);
}

WB_TEST(full_access) {
	/* Programs that can run anything, versioned names too. */
	for (const char *p : {"sh", "bash", "python3", "python3.13", "perl5.40", "lua5.4", "env",
			"xargs", "sudo", "pkexec", "find", "awk", "vim", "systemd-run", "busybox"})
		WB_CHECK(wb::full_access_run(p) == "it can run any command");
	for (const char *p : {"makoctl", "notify-send", "dunstctl", "swaync-client", "touch", ""})
		WB_CHECK(wb::full_access_run(p).empty());
	/* Files that run something later. */
	WB_CHECK(wb::full_access_file("~/.bashrc") == "it runs at your next login");
	WB_CHECK(wb::full_access_file("~/.config/waybox/autostart") == "Waybox runs what it says");
	WB_CHECK(wb::full_access_file("~/.config/waybox/approved-pages") ==
			"Waybox runs what it says");
	WB_CHECK(wb::full_access_file("~/.config/autostart/x.desktop") ==
			"it runs at your next login");
	WB_CHECK(wb::full_access_file("~/.local/bin/makoctl") == "it's run in place of a program");
	WB_CHECK(!wb::full_access_file("~/.ssh/authorized_keys").empty());
	/* Absolute, under the home folder. */
	WB_CHECK(wb::full_access_file("/home/u/.profile", "/home/u") == "it runs at your next login");
	WB_CHECK(wb::full_access_file("/home/user/.profile", "/home/u").empty());
	/* Other spellings of the same files: still said. */
	WB_CHECK(wb::full_access_file("//home/u/.bashrc", "/home/u") == "it runs at your next login");
	WB_CHECK(wb::full_access_file("~/.config//waybox/autostart") == "Waybox runs what it says");
	WB_CHECK(wb::full_access_file("~/.config/./autostart/x.desktop") ==
			"it runs at your next login");
	WB_CHECK(wb::full_access_file("~/.cache/../.profile") == "it runs at your next login");
	/* A program's own config: fine. */
	for (const char *f : {"~/.config/mako/config", "~/.config/waybar/config.jsonc",
			"~/.config/foot/foot.ini", "~/.bashrc.d", "/etc/x"})
		WB_CHECK(wb::full_access_file(f).empty());
	/* The checker says so. */
	auto c = wb::check_integration("x", "[Integration]\nName=x\nExec=x\nPage=x.lua\n"
			"Runs=makoctl, sh\nFiles=~/.config/x/config, ~/.profile\n");
	WB_CHECK(c.integration && has_problem(c, 0, false, "Runs=sh is full access") &&
			has_problem(c, 0, false, "Files=~/.profile is full access") &&
			!has_problem(c, 0, false, "Runs=makoctl"));
}

WB_TEST(page_access_tiers) {
	auto mako = *wb::parse_integration("mako", "[Integration]\nName=mako\nExec=mako\n");
	using wb::Reach;
	/* Its own program and companions; its own files. */
	WB_CHECK(wb::run_access(mako, "mako").reach == Reach::Own);
	WB_CHECK(wb::run_access(mako, "makoctl").reach == Reach::Own);
	WB_CHECK(wb::file_access(mako, "~/.config/mako/config").reach == Reach::Own);
	WB_CHECK(wb::file_access(mako, "/home/u/.config/mako/config", "/home/u").reach == Reach::Own);
	/* Anything else: another program, another's file; not "safe". */
	WB_CHECK(wb::run_access(mako, "notify-send").reach == Reach::Other);
	WB_CHECK(wb::run_access(mako, "mak").reach == Reach::Other);
	WB_CHECK(wb::run_access(mako, "mako-client").reach == Reach::Own);
	WB_CHECK(wb::run_access(mako, "makoish").reach == Reach::Other);
	/* A page claiming a short program doesn't make others its own. */
	auto s = *wb::parse_integration("s", "[Integration]\nName=s\nExec=s\n");
	WB_CHECK(wb::run_access(s, "s").reach == Reach::Own);
	WB_CHECK(wb::run_access(s, "sctl").reach == Reach::Own);
	for (const char *p : {"strace", "steam", "ssh-agent", "scp"})
		WB_CHECK(wb::run_access(s, p).reach != Reach::Own);
	/* Files: its folder where programs keep theirs, not any of that name. */
	WB_CHECK(wb::file_access(mako, "~/.local/share/mako/history").reach == Reach::Own);
	WB_CHECK(wb::file_access(mako, "~/.mako/x").reach == Reach::Own);
	WB_CHECK(wb::file_access(mako, "~/src/mako/x").reach == Reach::Other);
	WB_CHECK(wb::file_access(mako, "/etc/xdg/mako/config").reach == Reach::Own);
	WB_CHECK(wb::file_access(mako, "/etc/mako/config").reach == Reach::Other);
	WB_CHECK(wb::file_access(mako, "~/.config/mako/../foot/foot.ini").reach == Reach::Other);
	auto share = *wb::parse_integration("share", "[Integration]\nName=share\nExec=share\n");
	WB_CHECK(wb::file_access(share, "~/.local/share/applications/x.desktop").reach !=
			Reach::Own);
	WB_CHECK(wb::file_access(mako, "~/.config/makoish/config").reach == Reach::Other);
	WB_CHECK(wb::file_access(mako, "~/notes.txt").reach == Reach::Other);
	/* Known full access wins, even if named after the program. */
	for (const char *p : {"sh", "foot", "kitty", "xdg-open", "gdbus", "busctl", "wtype",
			"ydotool", "swaymsg"}) {
		auto a = wb::run_access(mako, p);
		WB_CHECK(a.reach == Reach::Full && a.why == "it can run any command");
	}
	auto bashlike = *wb::parse_integration("b", "[Integration]\nName=b\nExec=bash -c x\n");
	WB_CHECK(wb::run_access(bashlike, "bash").reach == Reach::Full);
	WB_CHECK(wb::file_access(mako, "~/.config/waybox/rc.xml").reach == Reach::Full);
}

/* A page edits a file its command names only once the user sets it: Exec=
 * naming one not in Files= is said. */
WB_TEST(integration_exec_names_file) {
	auto read = [](const std::string &exec, const std::string &files) {
		return wb::check_integration("x", "[Integration]\nName=x\nExec=" + exec +
				"\nPage=x.lua\nFiles=" + files + "\n\n[Option config]\nLabel=Config\n"
				"Flag=-c\nType=file\n");
	};
	WB_CHECK(has_problem(read("prog -c ~/.bashrc", "~/.config/x/config"), 0, false,
			"Exec= gives -c ~/.bashrc, which isn't in Files="));
	WB_CHECK(read("prog -c ~/.config/x/config", "~/.config/x/config").problems.empty());
	WB_CHECK(read("prog", "~/.config/x/config").problems.empty());
}

WB_TEST(integration_reload) {
	using K = wb::IntegrationReload::Kind;
	auto read = [](const std::string &reload) {
		return wb::check_integration("x", "[Integration]\nName=x\nExec=fakebar\nPage=x.lua\n"
				"Reload=" + reload + "\n");
	};
	auto c = read("command makoctl reload");
	WB_CHECK(c.integration && c.integration->reload.kind == K::Command &&
			(c.integration->reload.argv == std::vector<std::string>{"makoctl", "reload"}));
	c = read("signal USR2");
	WB_CHECK(c.integration->reload.kind == K::Signal && c.integration->reload.signal == "USR2");
	c = read("signal SIGHUP");
	WB_CHECK(c.integration->reload.kind == K::Signal && c.integration->reload.signal == "HUP");
	WB_CHECK(read("none").integration->reload.kind == K::None);
	WB_CHECK(read("restart").integration->reload.kind == K::Restart);
	/* Not one of those: said, and ignored. */
	for (const char *bad : {"signal KILL", "signal TERM", "signal", "command", "restart now",
			"x y"}) {
		c = read(bad);
		WB_CHECK(c.integration->reload.kind == K::Unset &&
				has_problem(c, 5, false, ("Reload=" + std::string(bad) + ": command PROGRAM").c_str()));
	}
	/* A command: a program's name; full access said. */
	WB_CHECK(has_problem(read("command /usr/bin/makoctl reload"), 0, false, "without a folder"));
	WB_CHECK(has_problem(read("command sh -c x"), 0, false, "full access"));
	/* Never Waybox. */
	auto w = wb::check_integration("w", "[Integration]\nName=w\nExec=waybox\nReload=signal USR1\n");
	WB_CHECK(has_problem(w, 0, true, "not to Waybox itself"));
	WB_CHECK(!read("command makoctl reload").integration->reload.argv.empty());
	WB_CHECK(wb::parse_integration("p", "[Integration]\nName=p\nExec=p\n")->reload.kind ==
			K::Unset);
}

