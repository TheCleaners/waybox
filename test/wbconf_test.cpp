#include "wb_test.hpp"

#include <string>

#include "waybox/wbconf.hpp"

using wb::RcDocument;
using wb::WayboxSettings;

static const char *kMinimalRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <keyboard>\n"
		"    <keybind key=\"A-Tab\"><action name=\"NextWindow\"/></keybind>\n"
		"  </keyboard>\n"
		"  <theme><name>Clearlooks</name></theme>\n"
		"</openbox_config>\n";

WB_TEST(reads_existing_values) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	WB_CHECK(s.theme_name.has_value() && *s.theme_name == "Clearlooks");
	/* Nothing else is present yet. */
	WB_CHECK(!s.titlebar_pad_y.has_value());
	WB_CHECK(!s.switcher_order.has_value());
	WB_CHECK(!s.margin_top.has_value());
}

WB_TEST(round_trips_all_settings) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());

	WayboxSettings s;
	s.theme_name = "Onyx-Citrus";
	s.titlebar_pad_y = 2;
	s.titlebar_button_size = 0;
	s.titlebar_resize_grab = 8;
	s.placement_policy = "Center";
	s.margin_top = 24;
	s.margin_bottom = 0;
	s.margin_left = 4;
	s.margin_right = 4;
	s.menu_source = "builtin";
	s.menu_submenu_open = "hover";
	s.menu_hover_delay = 120;
	s.menu_wrap = true;
	s.menu_icons = false;
	s.switcher_order = "mru";
	s.switcher_osd = true;
	s.switcher_wrap = false;
	doc->apply(s);

	/* Re-parse the serialised document and confirm every value survived. */
	std::string xml = doc->to_string();
	auto doc2 = RcDocument::from_string(xml);
	WB_CHECK(doc2.has_value());
	WayboxSettings r = doc2->read();
	WB_CHECK(r.theme_name && *r.theme_name == "Onyx-Citrus");
	WB_CHECK(r.titlebar_pad_y && *r.titlebar_pad_y == 2);
	WB_CHECK(r.titlebar_button_size && *r.titlebar_button_size == 0);
	WB_CHECK(r.titlebar_resize_grab && *r.titlebar_resize_grab == 8);
	WB_CHECK(r.placement_policy && *r.placement_policy == "Center");
	WB_CHECK(r.margin_top && *r.margin_top == 24);
	WB_CHECK(r.margin_left && *r.margin_left == 4);
	WB_CHECK(r.menu_source && *r.menu_source == "builtin");
	WB_CHECK(r.menu_submenu_open && *r.menu_submenu_open == "hover");
	WB_CHECK(r.menu_hover_delay && *r.menu_hover_delay == 120);
	WB_CHECK(r.menu_wrap && *r.menu_wrap == true);
	WB_CHECK(r.menu_icons && *r.menu_icons == false);
	WB_CHECK(r.switcher_order && *r.switcher_order == "mru");
	WB_CHECK(r.switcher_osd && *r.switcher_osd == true);
	WB_CHECK(r.switcher_wrap && *r.switcher_wrap == false);

	/* The unrelated keybinding must be preserved. */
	WB_CHECK(xml.find("NextWindow") != std::string::npos);
}

WB_TEST(updates_in_place_without_duplicating) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	s.theme_name = "Bear2";
	doc->apply(s);
	std::string xml = doc->to_string();
	/* The theme name was replaced, not duplicated. */
	WB_CHECK(xml.find("Bear2") != std::string::npos);
	WB_CHECK(xml.find("Clearlooks") == std::string::npos);
	auto cnt = [](const std::string &h, const std::string &n) {
		size_t c = 0, p = 0;
		while ((p = h.find(n, p)) != std::string::npos) { ++c; p += n.size(); }
		return c;
	};
	WB_CHECK(cnt(xml, "<name>") == 1);
}

WB_TEST(clearing_removes_nodes_and_attrs) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());

	WayboxSettings set;
	set.theme_name = "X";
	set.menu_icons = true;
	doc->apply(set);
	WB_CHECK(doc->to_string().find("icons=") != std::string::npos);

	/* Re-applying with the fields cleared removes them. */
	WayboxSettings cleared = doc->read();
	cleared.theme_name = std::nullopt;
	cleared.menu_icons = std::nullopt;
	doc->apply(cleared);
	std::string xml = doc->to_string();
	WB_CHECK(xml.find("icons=") == std::string::npos);
	WB_CHECK(xml.find("<name>") == std::string::npos);
}

WB_TEST(round_trips_fonts) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	s.font_active_window = "Inter Bold 12";
	s.font_menu_item = "Cantarell 11";
	doc->apply(s);

	std::string xml = doc->to_string();
	/* Structured font element written under <theme>. */
	WB_CHECK(xml.find("place=\"ActiveWindow\"") != std::string::npos);
	WB_CHECK(xml.find("<name>Inter</name>") != std::string::npos);
	WB_CHECK(xml.find("<size>12</size>") != std::string::npos);
	WB_CHECK(xml.find("<weight>bold</weight>") != std::string::npos);

	auto doc2 = RcDocument::from_string(xml);
	WB_CHECK(doc2.has_value());
	WayboxSettings r = doc2->read();
	WB_CHECK(r.font_active_window && *r.font_active_window == "Inter Bold 12");
	WB_CHECK(r.font_menu_item && *r.font_menu_item == "Cantarell 11");
	WB_CHECK(!r.font_osd.has_value());

	/* Clearing removes the font element. */
	r.font_active_window = std::nullopt;
	doc2->apply(r);
	WB_CHECK(doc2->to_string().find("place=\"ActiveWindow\"") == std::string::npos);
}

/* ---- Safety: strict loading, writing only what changed, safe saving ---- */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

WB_TEST(strict_load_refuses_broken_xml) {
	/* Each of these "parses" in libxml2's recover mode, dropping or changing
	 * part of the file; saving that would lose the user's config. */
	const char *broken[] = {
		/* a forgotten closing quote */
		"<openbox_config><keyboard><keybind key=\"W-y><action name=\"Close\"/>"
		"</keybind></keyboard><mouse><context name=\"Root\"/></mouse></openbox_config>",
		/* an unescaped & in a command */
		"<openbox_config><keyboard><keybind key=\"W-x\"><action name=\"Execute\">"
		"<command>a && b</command></action></keybind></keyboard></openbox_config>",
		/* not XML */
		"not xml at all",
		/* truncated */
		"<openbox_config><keyboard>",
	};
	for (const char *xml : broken) {
		wb::LoadError err;
		WB_CHECK(!RcDocument::from_string(xml, &err).has_value());
		WB_CHECK(!err.message.empty());
	}
	wb::LoadError err;
	RcDocument::from_string("<a>\n<b>\n</a>", &err);
	WB_CHECK(err.line >= 2);
}

static const char *kUserRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <placement><policy>Centre</policy></placement>\n"
		"  <theme>\n"
		"    <name>Clearlooks</name>\n"
		"    <font place=\"OnScreenDisplay\"><name>3270 Nerd Font</name><size>11</size></font>\n"
		"  </theme>\n"
		"</openbox_config>\n";

WB_TEST(apply_writes_only_what_changed) {
	auto doc = RcDocument::from_string(kUserRc);
	WB_CHECK(doc.has_value());
	/* What an editor shows: the file's values, defaults for the rest. */
	WayboxSettings shown = doc->read();
	shown.placement_policy = "Smart";  /* "Centre" isn't in its list */
	shown.margin_top = 0;
	shown.titlebar_pad_y = 2;
	shown.menu_wrap = true;
	WayboxSettings edited = shown;
	edited.margin_left = 7;
	doc->apply(edited, shown);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<left>7</left>") != std::string::npos);
	/* Untouched: neither the shown defaults nor the value it couldn't show. */
	WB_CHECK(out.find("<policy>Centre</policy>") != std::string::npos);
	WB_CHECK(out.find("<top>") == std::string::npos);
	WB_CHECK(out.find("paddingY") == std::string::npos);
	WB_CHECK(out.find("wrap") == std::string::npos);
	WB_CHECK(out.find("<name>3270 Nerd Font</name>") != std::string::npos);
}

WB_TEST(fonts_keep_numbers_in_family_and_their_place) {
	auto doc = RcDocument::from_string(kUserRc);
	WayboxSettings s = doc->read();
	/* The OSD font under another accepted place name is found... */
	WB_CHECK(s.font_osd == std::optional<std::string>("3270 Nerd Font 11"));
	/* ...and written back into the same element. */
	WayboxSettings before = s;
	s.font_osd = "3270 Nerd Font Bold 12";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<name>3270 Nerd Font</name>") != std::string::npos);
	WB_CHECK(out.find("<size>12</size>") != std::string::npos);
	WB_CHECK(out.find("ActiveOnScreenDisplay") == std::string::npos);
	size_t first = out.find("<font ");
	WB_CHECK(first != std::string::npos && out.find("<font ", first + 1) == std::string::npos);
}

static std::string slurp(const std::filesystem::path &p) {
	std::ifstream in(p);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

WB_TEST(save_keeps_a_backup_the_symlink_and_the_mode) {
	namespace fs = std::filesystem;
	char tmpl[] = "/tmp/wbconf-test-XXXXXX";
	WB_CHECK(mkdtemp(tmpl) != nullptr);
	fs::path dir = tmpl;
	fs::path real = dir / "dotfiles" / "rc.xml";
	fs::create_directories(real.parent_path());
	{
		std::ofstream(real) << kUserRc;
	}
	::chmod(real.c_str(), 0600);
	fs::path link = dir / "rc.xml";
	fs::create_symlink(real, link);

	auto doc = RcDocument::load_file(link.string());
	WB_CHECK(doc.has_value());
	WayboxSettings before = doc->read(), s = before;
	s.margin_top = 9;
	doc->apply(s, before);
	std::string err;
	WB_CHECK(doc->save_file(link.string(), &err));
	WB_CHECK(err.empty());

	WB_CHECK(fs::is_symlink(link));                       /* the link survives */
	WB_CHECK(slurp(real).find("<top>9</top>") != std::string::npos);
	WB_CHECK(slurp(fs::path(real.string() + ".bak")) == kUserRc);  /* old file kept */
	struct stat st {};
	WB_CHECK(::stat(real.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
	WB_CHECK(!fs::exists(real.string() + ".tmp"));
	fs::remove_all(dir);
}

/* ---- Desktops, edge tiling, monitors ---- */

static const char *kMoreRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <desktops><number>3</number>"
		"<names><name>Mail</name><name>Code</name></names></desktops>\n"
		"  <waybox>\n"
		"    <edgeTiling enabled=\"no\" range=\"12\"/>\n"
		"    <outputs>\n"
		"      <!-- the TV -->\n"
		"      <output name=\"HDMI-A-1\" mode=\"3840x2160@60\" scale=\"1.5\"/>\n"
		"      <output name=\"DP-4\" position=\"3840,0\"/>\n"
		"      <output make=\"Dell Inc.\" name=\"DP-9\" scale=\"2\"/>\n"
		"    </outputs>\n"
		"  </waybox>\n"
		"</openbox_config>\n";

WB_TEST(reads_desktops_edge_tiling_and_outputs) {
	auto doc = RcDocument::from_string(kMoreRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	WB_CHECK(s.desktops_number == std::optional<int>(3));
	WB_CHECK(s.desktop_names && s.desktop_names->size() == 2 &&
			(*s.desktop_names)[1] == "Code");
	WB_CHECK(!s.desktop_popup_time.has_value());
	WB_CHECK(s.edge_tiling == std::optional<bool>(false));
	WB_CHECK(s.edge_tiling_range == std::optional<int>(12));
	/* Only the rules matched by name alone. */
	WB_CHECK(s.outputs.size() == 2);
	WB_CHECK(s.outputs[0].name == "HDMI-A-1" &&
			s.outputs[0].attrs.at("mode") == "3840x2160@60" &&
			s.outputs[0].attrs.at("scale") == "1.5");
	WB_CHECK(s.outputs[1].name == "DP-4" && s.outputs[1].attrs.size() == 1);
}

WB_TEST(writes_only_the_output_setting_that_changed) {
	auto doc = RcDocument::from_string(kMoreRc);
	WayboxSettings before = doc->read(), s = before;
	s.outputs[0].attrs["scale"] = "2";           /* change one attribute */
	s.outputs[1].attrs.clear();                   /* DP-4 back to automatic */
	s.outputs.push_back({"DP-1", {{"enabled", "no"}}});  /* a new one */
	s.desktop_popup_time = 0;
	s.desktop_names = std::vector<std::string>{"Mail", "Code", "Games"};
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("mode=\"3840x2160@60\" scale=\"2\"") != std::string::npos);
	WB_CHECK(out.find("DP-4") == std::string::npos);  /* no settings left */
	WB_CHECK(out.find("<output name=\"DP-1\" enabled=\"no\"/>") != std::string::npos);
	WB_CHECK(out.find("make=\"Dell Inc.\" name=\"DP-9\" scale=\"2\"") != std::string::npos);
	WB_CHECK(out.find("<!-- the TV -->") != std::string::npos);
	WB_CHECK(out.find("<popupTime>0</popupTime>") != std::string::npos);
	WB_CHECK(out.find("<name>Games</name>") != std::string::npos);
	WB_CHECK(out.find("edgeTiling enabled=\"no\" range=\"12\"") != std::string::npos);

	/* Read back: what was written is what is read. */
	auto again = RcDocument::from_string(out);
	WayboxSettings r = again->read();
	WB_CHECK(r.outputs.size() == 2 && r.outputs[0].attrs.at("scale") == "2" &&
			r.outputs[1].name == "DP-1");
	WB_CHECK(r.desktop_names && r.desktop_names->size() == 3);
}

/* ---- Keyboard, mouse, HDR and advanced settings ---- */

static const char *kInputRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <keyboard>\n"
		"    <xkb><layout>us,de</layout><options>grp:alt_shift_toggle</options></xkb>\n"
		"    <keybind key=\"W-x\"><action name=\"Close\"/></keybind>\n"
		"  </keyboard>\n"
		"  <mouse>\n"
		"    <libinput><naturalScroll>enabled</naturalScroll><accelSpeed>0.3</accelSpeed></libinput>\n"
		"    <context name=\"Root\"/>\n"
		"  </mouse>\n"
		"  <waybox><hdr outputs=\"HDMI-A-1\"/><xwayland enabled=\"no\"/></waybox>\n"
		"</openbox_config>\n";

WB_TEST(reads_keyboard_mouse_hdr_advanced) {
	auto doc = RcDocument::from_string(kInputRc);
	WayboxSettings s = doc->read();
	WB_CHECK(s.xkb.size() == 2 && s.xkb.at("layout") == "us,de" &&
			s.xkb.at("options") == "grp:alt_shift_toggle");
	WB_CHECK(s.libinput.size() == 2 && s.libinput.at("naturalScroll") == "enabled" &&
			s.libinput.at("accelSpeed") == "0.3");
	WB_CHECK(s.hdr_outputs == std::optional<std::string>("HDMI-A-1"));
	WB_CHECK(s.xwayland == std::optional<bool>(false));
	WB_CHECK(!s.virtual_input.has_value());
}

WB_TEST(writes_only_changed_input_settings) {
	auto doc = RcDocument::from_string(kInputRc);
	WayboxSettings before = doc->read(), s = before;
	s.xkb.erase("options");                     /* back to the default */
	s.libinput["tap"] = "enabled";              /* a new one */
	s.libinput["accelSpeed"] = "-0.2";          /* a changed one */
	s.virtual_input = "never";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("grp:alt_shift_toggle") == std::string::npos);
	WB_CHECK(out.find("<layout>us,de</layout>") != std::string::npos);
	WB_CHECK(out.find("<accelSpeed>-0.2</accelSpeed>") != std::string::npos);
	WB_CHECK(out.find("<tap>enabled</tap>") != std::string::npos);
	WB_CHECK(out.find("<naturalScroll>enabled</naturalScroll>") != std::string::npos);
	WB_CHECK(out.find("virtualInput allow=\"never\"") != std::string::npos);
	/* Bindings next to them are untouched. */
	WB_CHECK(out.find("<keybind key=\"W-x\">") != std::string::npos);
	WB_CHECK(out.find("<context name=\"Root\"/>") != std::string::npos);

	/* Everything back to defaults: the <xkb> and <libinput> blocks go. */
	WayboxSettings b2 = s, s2 = s;
	s2.xkb.clear();
	s2.libinput.clear();
	doc->apply(s2, b2);
	out = doc->to_string();
	WB_CHECK(out.find("<xkb") == std::string::npos);
	WB_CHECK(out.find("<libinput") == std::string::npos);
	WB_CHECK(out.find("<keybind key=\"W-x\">") != std::string::npos);
}

WB_TEST(title_layout_round_trip) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WayboxSettings before = doc->read(), s = before;
	WB_CHECK(!before.title_layout.has_value());
	s.title_layout = "NLSDIMC";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<titleLayout>NLSDIMC</titleLayout>") != std::string::npos);
	WB_CHECK(out.find("<name>Clearlooks</name>") != std::string::npos);
	WB_CHECK(RcDocument::from_string(out)->read().title_layout ==
			std::optional<std::string>("NLSDIMC"));
}

WB_TEST(switcher_icons_round_trip) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WayboxSettings before = doc->read(), s = before;
	s.switcher_icons = false;
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("icons=\"no\"") != std::string::npos);
	WB_CHECK(RcDocument::from_string(out)->read().switcher_icons ==
			std::optional<bool>(false));
}

WB_TEST(output_left_out_is_removed) {
	/* What wbconf does for a monitor set back to Automatic: it isn't in
	 * the settings any more. */
	auto doc = RcDocument::from_string(kMoreRc);
	WayboxSettings before = doc->read(), s = before;
	s.outputs.erase(s.outputs.begin());  /* HDMI-A-1 */
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("HDMI-A-1") == std::string::npos);
	WB_CHECK(out.find("<output name=\"DP-4\" position=\"3840,0\"/>") != std::string::npos);
	WB_CHECK(out.find("make=\"Dell Inc.\"") != std::string::npos);
}

WB_TEST(startup_programs_read_and_written_in_place) {
	auto doc = RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <waybox>\n"
			"    <startup>\n"
			"      <!-- my panel -->\n"
			"      <program name=\"panel\" restart=\"no\">waybar</program>\n"
			"      <program>/usr/bin/mako</program>\n"
			"      <program enabled=\"no\">wlsunset -l 1 -L 2</program>\n"
			"    </startup>\n"
			"    <idle screenOff=\"600\"/>\n"
			"  </waybox>\n"
			"</openbox_config>\n");
	WayboxSettings before = doc->read(), s = before;
	WB_CHECK(s.startup.size() == 3);
	WB_CHECK(s.startup[0].name == "panel" && s.startup[0].attrs.at("restart") == "no");
	WB_CHECK(s.startup[1].name == "mako" && s.startup[1].command == "/usr/bin/mako");
	WB_CHECK(s.startup[2].attrs.at("enabled") == "no");
	WB_CHECK(s.idle_screen_off == std::optional<int>(600));

	/* Turn wlsunset on with a new location, drop mako, add swaybg. */
	s.startup[2].attrs.erase("enabled");
	s.startup[2].command = "wlsunset -l 48.8 -L 2.3";
	s.startup.erase(s.startup.begin() + 1);
	s.startup.push_back({"wall", "swaybg -c '#333' & echo <x>", {{"name", "wall"}}});
	s.idle_screen_off.reset();
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<!-- my panel -->") != std::string::npos);  /* untouched */
	WB_CHECK(out.find("<program name=\"panel\" restart=\"no\">waybar</program>") !=
			std::string::npos);
	WB_CHECK(out.find("mako") == std::string::npos);
	WB_CHECK(out.find("<program>wlsunset -l 48.8 -L 2.3</program>") != std::string::npos);
	WB_CHECK(out.find("swaybg -c '#333' &amp; echo &lt;x&gt;") != std::string::npos);
	WB_CHECK(out.find("screenOff") == std::string::npos);
	WayboxSettings again = RcDocument::from_string(out)->read();
	WB_CHECK(again.startup == s.startup);
}

WB_TEST(startup_emptied_is_removed) {
	auto doc = RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <waybox><startup><program>waybar</program></startup></waybox>\n"
			"</openbox_config>\n");
	WayboxSettings before = doc->read(), s = before;
	s.startup.clear();
	doc->apply(s, before);
	WB_CHECK(doc->to_string().find("startup") == std::string::npos);
	/* And nothing written when untouched. */
	auto doc2 = RcDocument::from_string(kMinimalRc);
	WayboxSettings b2 = doc2->read();
	doc2->apply(b2, b2);
	WB_CHECK(doc2->to_string().find("waybox") == std::string::npos);
}
