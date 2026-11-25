#include "wb_test.hpp"

#include "waybox/integrations.hpp"

#include <cstdio>
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
