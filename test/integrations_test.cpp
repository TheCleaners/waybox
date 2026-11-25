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
